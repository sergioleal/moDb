#include "target_embedded.hpp"

#include "dataset_user.hpp"
#include "process_metrics.hpp"
#include "runner/json_util.hpp"
#include "runner/sha256.hpp"

#include "modb/object/database.hpp"
#include "modb/storage/page.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <sstream>
#include <vector>

using namespace modb;
using namespace modb::object;
using modb::bench::percentile_sorted;
using modb::bench::sha256_hex;
using modb::bench::sha256_text;

namespace modb::loadtest {
namespace {

// Struct C++ + Binding do tipo `User` (§7). Vive só nesta unidade de
// compilação -- o mesmo padrão de `object_store_lifecycle.cpp` para `Item`.
struct User {
    std::int64_t id{};
    std::string login;
    std::string email;
    std::string display_name;
    std::int64_t created_at{};
    std::int32_t status{};
    std::vector<std::byte> filler;
};

BindingBuilder<User> user_binding() {
    BindingBuilder<User> builder{"User"};
    builder.field<1>("id", &User::id)
        .field<2>("login", &User::login)
        .field<3>("email", &User::email)
        .field<4>("display_name", &User::display_name)
        .field<5>("created_at", &User::created_at)
        .field<6>("status", &User::status)
        .field<7>("filler", &User::filler);
    return builder;
}

User to_engine_user(const GeneratedUser& g) {
    User u;
    u.id = g.id;
    u.login = g.login;
    u.email = g.email;
    u.display_name = g.display_name;
    u.created_at = g.created_at;
    u.status = g.status;
    u.filler = g.filler;
    return u;
}

GeneratedUser from_engine_user(const User& u) {
    GeneratedUser g;
    g.id = u.id;
    g.login = u.login;
    g.email = u.email;
    g.display_name = u.display_name;
    g.created_at = u.created_at;
    g.status = u.status;
    g.filler = u.filler;
    return g;
}

std::uint64_t ns_between(std::chrono::steady_clock::time_point a,
                         std::chrono::steady_clock::time_point b) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// Bytes lógicos de um GeneratedUser: os campos como o chamador os vê, não
// como o motor os codifica em página (usado para write/space amplification,
// §8) -- deliberadamente diferente de `canonical_line`, que serve só ao hash
// e carrega separadores decorativos que inflariam a contagem.
std::uint64_t logical_size_bytes(const GeneratedUser& u) {
    return sizeof(u.id) + u.login.size() + u.email.size() + u.display_name.size() +
          sizeof(u.created_at) + sizeof(u.status) + u.filler.size();
}

// Garante detach do registro e destruição correta do Database mesmo em
// caminho de erro (RAII, igual ao AttachedDatabase do benchmark).
struct AttachedDatabase {
    std::shared_ptr<Database> database;
    DatabaseId id{};

    ~AttachedDatabase() {
        if (id.value != 0) {
            DatabaseRegistry::instance().detach(id);
        }
        database.reset();
    }
};

} // namespace

CaseRunResult run_create_only_embedded(const WorkloadParams& params,
                                       std::filesystem::path& out_db_path) {
    CaseRunResult result;

    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path db_path =
        std::filesystem::path{params.work_dir} /
        ("modb_load-create_only-" + std::to_string(params.seed) + "-" + std::to_string(unique) +
         ".modb");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    AttachedDatabase attached;
    auto created = Database::create(db_path);
    if (!created) {
        result.status = "failed";
        result.error = "Database::create: " + created.error().message;
        return result;
    }
    attached.database = std::make_shared<Database>(std::move(*created));
    auto database_id = DatabaseRegistry::instance().attach(attached.database);
    if (!database_id) {
        result.status = "failed";
        result.error = "DatabaseRegistry::attach: " + database_id.error().message;
        return result;
    }
    attached.id = *database_id;

    if (auto bound = attached.database->bind(user_binding()); !bound) {
        result.status = "failed";
        result.error = "bind(User): " + bound.error().message;
        return result;
    }

    // Tamanho antes de qualquer objeto de usuário (cabeçalho/catálogo) --
    // referência para estimar páginas escritas pela fase (§8: pages_written
    // não tem contador real no motor hoje, só esta estimativa por bytes).
    std::error_code size_error;
    const auto initial_bytes = std::filesystem::file_size(db_path, size_error);
    const std::uint64_t db_bytes_before = size_error ? 0 : initial_bytes;
    const auto pages_read_before = attached.database->data_pages_read();

    const auto batch = params.batch == 0 ? params.object_count : params.batch;
    std::vector<ObjectId> ids;
    ids.reserve(params.object_count);
    std::ostringstream expected;
    std::vector<double> latencies_ns;
    latencies_ns.reserve(params.object_count);
    std::uint64_t logical_bytes = 0;
    std::uint64_t errors = 0;

    // `Transaction` só tem construtor de movimento (a atribuição de movimento é
    // deletada de propósito, database.hpp:87), então trocar de transação a
    // cada lote usa `std::optional<Transaction>::emplace`, nunca `tx = ...`.
    auto first_tx = attached.database->begin();
    if (!first_tx) {
        result.status = "failed";
        result.error = "begin(create): " + first_tx.error().message;
        return result;
    }
    std::optional<Transaction> tx{std::move(*first_tx)};

    const auto create_start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 1; i <= params.object_count; ++i) {
        const auto generated = generate_user(params.seed, i, params.payload);
        expected << canonical_line(generated) << '\n';
        logical_bytes += logical_size_bytes(generated);

        const auto op_start = std::chrono::steady_clock::now();
        auto created_handle = attached.database->create(*tx, to_engine_user(generated));
        const auto op_end = std::chrono::steady_clock::now();
        latencies_ns.push_back(static_cast<double>(ns_between(op_start, op_end)));

        if (!created_handle) {
            ++errors;
            result.status = "failed";
            result.error = "create(User, i=" + std::to_string(i) +
                           "): " + created_handle.error().message;
            return result;
        }
        ids.push_back(created_handle->id());

        if (i % batch == 0 || i == params.object_count) {
            if (auto committed = tx->commit(); !committed) {
                result.status = "failed";
                result.error = "commit(create): " + committed.error().message;
                return result;
            }
            if (i != params.object_count) {
                auto next_tx = attached.database->begin();
                if (!next_tx) {
                    result.status = "failed";
                    result.error = "begin(create, lote seguinte): " + next_tx.error().message;
                    return result;
                }
                tx.emplace(std::move(*next_tx));
            }
        }
    }
    const auto create_end = std::chrono::steady_clock::now();
    const auto pages_read_after = attached.database->data_pages_read();

    const auto file_bytes = std::filesystem::file_size(db_path, size_error);
    result.peak_disk_bytes = size_error ? 0 : file_bytes;

    std::error_code wal_size_error;
    const auto wal_bytes = std::filesystem::file_size(wal_path, wal_size_error);

    std::sort(latencies_ns.begin(), latencies_ns.end());
    LatencyPercentilesNs latency;
    if (!latencies_ns.empty()) {
        latency.p50 = percentile_sorted(latencies_ns, 0.50);
        latency.p95 = percentile_sorted(latencies_ns, 0.95);
        latency.p99 = percentile_sorted(latencies_ns, 0.99);
        latency.p999 = percentile_sorted(latencies_ns, 0.999);
    }

    const auto bytes_written = result.peak_disk_bytes > db_bytes_before
                                  ? result.peak_disk_bytes - db_bytes_before
                                  : 0;

    const auto create_ns = ns_between(create_start, create_end);
    PhaseMetrics create_phase;
    create_phase.phase = "create";
    create_phase.operations = params.object_count;
    create_phase.duration_ns = create_ns;
    create_phase.ops_per_second =
        create_ns > 0 ? (static_cast<double>(params.object_count) * 1'000'000'000.0) /
                            static_cast<double>(create_ns)
                     : 0.0;
    create_phase.bytes_per_object =
        params.object_count > 0 ? result.peak_disk_bytes / params.object_count : 0;
    create_phase.errors = errors;
    create_phase.latency_ns = latency;
    create_phase.peak_rss_bytes = peak_rss_bytes();
    create_phase.db_bytes = result.peak_disk_bytes;
    create_phase.wal_bytes = wal_size_error ? 0 : wal_bytes;
    create_phase.pages_read = pages_read_after - pages_read_before;
    create_phase.pages_written_estimated = bytes_written / modb::storage::page_size;
    result.phases.push_back(create_phase);

    // Validação (§9): reler todos os objetos criados e comparar o hash lógico
    // observado com o esperado pelo gerador -- não só a contagem.
    std::ostringstream actual;
    std::uint64_t read_errors = 0;
    for (const auto id : ids) {
        auto handle = attached.database->get<User>(id);
        if (!handle) {
            ++read_errors;
            continue;
        }
        auto value = attached.database->materialize(*handle);
        if (!value) {
            ++read_errors;
            continue;
        }
        actual << canonical_line(from_engine_user(*value)) << '\n';
    }

    result.expected_hash = sha256_hex(sha256_text(expected.str()));
    result.actual_hash = sha256_hex(sha256_text(actual.str()));
    result.hash_match = read_errors == 0 && ids.size() == params.object_count &&
                        result.expected_hash == result.actual_hash;

    if (!result.hash_match) {
        result.status = "failed";
        result.error = "validação de hash lógico falhou (" + std::to_string(read_errors) +
                       " erro(s) de releitura, " + std::to_string(ids.size()) + "/" +
                       std::to_string(params.object_count) + " objetos)";
        return result;
    }

    result.total_duration_ns = create_ns;
    result.write_amplification = logical_bytes > 0
                                    ? static_cast<double>(bytes_written) /
                                          static_cast<double>(logical_bytes)
                                    : 0.0;
    result.space_amplification = logical_bytes > 0
                                    ? static_cast<double>(result.peak_disk_bytes) /
                                          static_cast<double>(logical_bytes)
                                    : 0.0;
    result.status = "completed";
    result.ok = true;
    return result;
}

} // namespace modb::loadtest
