#include "target_client.hpp"

#include "dataset_user.hpp"
#include "loadtest_facade.hpp"
#include "process_metrics.hpp"
#include "runner/json_util.hpp"
#include "runner/sha256.hpp"
#include "user_type.hpp"

#include "modb/app/server_connection.hpp"
#include "modb/net/query_description.hpp"
#include "modb/net/server.hpp"
#include "modb/object/object_codec.hpp"
#include "modb/ops/facade_catalog.hpp"
#include "modb/ops/module_manifest.hpp"
#include "modb/ops/operation_registry.hpp"
#include "modb/storage/page.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>
#include <thread>

using modb::bench::percentile_sorted;
using modb::bench::sha256_hex;
using modb::bench::sha256_text;

namespace modb::loadtest {
namespace {

std::uint64_t ns_between(std::chrono::steady_clock::time_point a,
                         std::chrono::steady_clock::time_point b) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

std::uint64_t logical_size_bytes(const GeneratedUser& u) {
    return sizeof(u.id) + u.login.size() + u.email.size() + u.display_name.size() +
          sizeof(u.created_at) + sizeof(u.status) + u.filler.size();
}

std::filesystem::path make_client_db_path(const WorkloadParams& params,
                                          std::string_view workload_tag) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::path{params.work_dir} /
          ("modb_load-" + std::string{workload_tag} + "-" + std::to_string(params.seed) + "-" +
           std::to_string(unique) + ".modb");
}

} // namespace

CaseRunResult run_create_only_client(const WorkloadParams& params,
                                     std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_client_db_path(params, "create_only_loopback");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    auto server = net::Server::listen(db_path, "127.0.0.1", 0);
    if (!server) {
        result.status = "failed";
        result.error = "Server::listen: " + server.error().message;
        return result;
    }
    if (auto bound = server->database().bind(user_binding()); !bound) {
        result.status = "failed";
        result.error = "bind(User) no servidor: " + bound.error().message;
        return result;
    }
    const auto type_id = server->database().type_id_of<User>();
    if (!type_id) {
        result.status = "failed";
        result.error = "type_id_of<User>: " + type_id.error().message;
        return result;
    }

    auto registry = std::make_shared<ops::OperationRegistry>();
    auto catalog = std::make_shared<ops::FacadeCatalog>();
    ops::ModuleLoader loader;
    const auto baseline = server->database().current_baseline()
                             ? server->database().current_baseline()->id()
                             : object::BaselineId{};
    const auto manifest = loadtest_facade_manifest(baseline);
    loader.admit_hash(manifest.hash);
    auto loaded = loader.load(manifest, baseline, *registry, *catalog,
                              [](ops::OperationRegistry& reg) {
                                  return register_loadtest_facade_module(reg);
                              });
    if (!loaded) {
        result.status = "failed";
        result.error = "ModuleLoader::load: " + loaded.error().message;
        return result;
    }
    server->set_operation_registry(registry);
    server->set_facade_catalog(catalog);

    const auto pages_read_before = server->database().data_pages_read();

    // A partir daqui um `std::thread` fica bloqueado em `accept()` dentro de
    // `serve_one()` -- todo caminho de saída abaixo precisa terminar com
    // `acceptor.join()`. Se o `connect()` do cliente falhar, ninguém jamais
    // aceita a conexão; `request_stop()` fecha o listener para destravar o
    // `accept()` nesse caso específico (única situação em que o par
    // conecta/aceita não completa por conta própria).
    std::thread acceptor([&server] { (void)server->serve_one(); });

    bool ok = false;
    std::string error_message;
    std::vector<double> batch_latencies_ns;
    std::uint64_t total_create_ns = 0;
    std::string expected_hash, actual_hash;
    bool hash_match = false;
    std::uint64_t logical_bytes = 0;

    {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto connection = app::ServerConnection::connect({
            .host = "127.0.0.1",
            .port = server->port(),
            .database_name = std::string{server->database_name()},
        });
        if (!connection) {
            error_message = "ServerConnection::connect: " + connection.error().message;
            server->request_stop();
        } else {
            auto handle = connection->open_facade<LoadtestFacade>();
            if (!handle) {
                error_message = "open_facade<LoadtestFacade>: " + handle.error().message;
            } else {
                const auto batch = params.batch == 0 ? params.object_count : params.batch;
                bool batches_ok = true;
                const auto create_start = std::chrono::steady_clock::now();
                for (std::uint64_t start = 0; start < params.object_count && batches_ok;
                    start += batch) {
                    const auto count = std::min<std::uint64_t>(batch, params.object_count - start);
                    const auto op_start = std::chrono::steady_clock::now();
                    auto invoked = handle->invoke<CreateBatch>(params.seed, start + 1, count,
                                                               params.payload);
                    const auto op_end = std::chrono::steady_clock::now();
                    batch_latencies_ns.push_back(static_cast<double>(ns_between(op_start, op_end)));
                    if (!invoked) {
                        error_message = "CreateBatch(start=" + std::to_string(start + 1) +
                                       "): " + invoked.error().message;
                        batches_ok = false;
                    }
                }
                const auto create_end = std::chrono::steady_clock::now();
                total_create_ns = ns_between(create_start, create_end);

                if (batches_ok) {
                    // Validação (§9): relê TUDO via query remota -- não os
                    // ids devolvidos pelo create (CreateBatch não devolve
                    // nenhum), e ordena pelo campo lógico `id` antes de
                    // comparar, porque a ordem de um full scan por rede não é
                    // garantida ser a ordem de criação (ao contrário do loop
                    // sequencial embedded, que relê pelos próprios ids na
                    // ordem em que foram criados).
                    auto collected =
                        connection->collect(net::QueryDescription{.type = *type_id, .limit = 0});
                    if (!collected) {
                        error_message = "collect(User): " + collected.error().message;
                    } else {
                        auto binding = user_binding().build();
                        if (!binding) {
                            error_message = "user_binding().build(): " + binding.error().message;
                        } else {
                            std::vector<GeneratedUser> actual_users;
                            actual_users.reserve(collected->size());
                            bool decode_ok = true;
                            for (const auto& decoded : *collected) {
                                User u;
                                if (auto materialized = binding->materialize(decoded.fields, &u);
                                    !materialized) {
                                    error_message = "Binding::materialize: " +
                                                   materialized.error().message;
                                    decode_ok = false;
                                    break;
                                }
                                actual_users.push_back(from_engine_user(u));
                            }
                            if (decode_ok) {
                                std::sort(actual_users.begin(), actual_users.end(),
                                         [](const GeneratedUser& a, const GeneratedUser& b) {
                                             return a.id < b.id;
                                         });
                                std::ostringstream actual_stream;
                                for (const auto& u : actual_users) {
                                    actual_stream << canonical_line(u) << '\n';
                                }
                                actual_hash = sha256_hex(sha256_text(actual_stream.str()));

                                std::ostringstream expected_stream;
                                for (std::uint64_t i = 1; i <= params.object_count; ++i) {
                                    const auto generated =
                                        generate_user(params.seed, i, params.payload);
                                    logical_bytes += logical_size_bytes(generated);
                                    expected_stream << canonical_line(generated) << '\n';
                                }
                                expected_hash = sha256_hex(sha256_text(expected_stream.str()));
                                hash_match = actual_users.size() == params.object_count &&
                                            actual_hash == expected_hash;
                                ok = hash_match;
                                if (!ok) {
                                    error_message = "validação de hash lógico falhou (" +
                                                   std::to_string(actual_users.size()) + "/" +
                                                   std::to_string(params.object_count) +
                                                   " objetos lidos de volta)";
                                }
                            }
                        }
                    }
                }
            }
        }
    } // `connection` sai de escopo aqui -- fecha a sessão e libera `serve_one()`.

    acceptor.join();

    if (!ok) {
        result.status = "failed";
        result.error = error_message;
        return result;
    }

    std::error_code size_ec;
    const auto db_bytes = std::filesystem::file_size(db_path, size_ec);
    const std::uint64_t db_bytes_after = size_ec ? 0 : db_bytes;
    std::error_code wal_ec;
    const auto wal_bytes = std::filesystem::file_size(wal_path, wal_ec);
    const auto pages_read_after = server->database().data_pages_read();

    PhaseMetrics phase;
    phase.phase = "create";
    phase.operations = params.object_count;
    phase.duration_ns = total_create_ns;
    phase.ops_per_second = total_create_ns > 0
                             ? (static_cast<double>(params.object_count) * 1'000'000'000.0) /
                                   static_cast<double>(total_create_ns)
                             : 0.0;
    phase.bytes_per_object = params.object_count > 0 ? db_bytes_after / params.object_count : 0;
    phase.errors = 0;
    // Granularidade de latência por LOTE, não por objeto -- cada `invoke` é
    // uma viagem de rede só, ao contrário do `target_embedded`, que mede
    // cada `create()` individualmente. Não comparável ponto a ponto com o
    // p50/p99 do alvo `embedded` para o mesmo caso.
    std::sort(batch_latencies_ns.begin(), batch_latencies_ns.end());
    if (!batch_latencies_ns.empty()) {
        phase.latency_ns.p50 = percentile_sorted(batch_latencies_ns, 0.50);
        phase.latency_ns.p95 = percentile_sorted(batch_latencies_ns, 0.95);
        phase.latency_ns.p99 = percentile_sorted(batch_latencies_ns, 0.99);
        phase.latency_ns.p999 = percentile_sorted(batch_latencies_ns, 0.999);
    }
    phase.peak_rss_bytes = peak_rss_bytes();   // processo combinado cliente+servidor (mesmo processo)
    phase.db_bytes = db_bytes_after;
    phase.wal_bytes = wal_ec ? 0 : wal_bytes;
    phase.pages_read = pages_read_after - pages_read_before;
    phase.pages_written_estimated = db_bytes_after / modb::storage::page_size;
    result.phases.push_back(phase);

    result.expected_hash = expected_hash;
    result.actual_hash = actual_hash;
    result.hash_match = hash_match;
    result.peak_disk_bytes = db_bytes_after;
    result.total_duration_ns = total_create_ns;
    result.write_amplification =
        logical_bytes > 0 ? static_cast<double>(db_bytes_after) / static_cast<double>(logical_bytes)
                          : 0.0;
    result.space_amplification = result.write_amplification;
    result.status = "completed";
    result.ok = true;
    return result;
}

} // namespace modb::loadtest
