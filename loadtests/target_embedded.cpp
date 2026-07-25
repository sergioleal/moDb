#include "target_embedded.hpp"

#include "dataset_user.hpp"
#include "process_metrics.hpp"
#include "runner/json_util.hpp"
#include "runner/sha256.hpp"

#include "modb/object/database.hpp"
#include "modb/storage/page.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
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

LatencyPercentilesNs percentiles_of(std::vector<double> latencies_ns) {
    std::sort(latencies_ns.begin(), latencies_ns.end());
    LatencyPercentilesNs latency;
    if (!latencies_ns.empty()) {
        latency.p50 = percentile_sorted(latencies_ns, 0.50);
        latency.p95 = percentile_sorted(latencies_ns, 0.95);
        latency.p99 = percentile_sorted(latencies_ns, 0.99);
        latency.p999 = percentile_sorted(latencies_ns, 0.999);
    }
    return latency;
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

struct SetupResult {
    bool ok{};
    std::string error;
};

// Cria o arquivo, registra e faz bind de `User` -- comum a todo workload
// embedded desta subfase.
SetupResult setup_database(AttachedDatabase& attached, const std::filesystem::path& db_path) {
    SetupResult result;
    auto created = Database::create(db_path);
    if (!created) {
        result.error = "Database::create: " + created.error().message;
        return result;
    }
    attached.database = std::make_shared<Database>(std::move(*created));
    auto database_id = DatabaseRegistry::instance().attach(attached.database);
    if (!database_id) {
        result.error = "DatabaseRegistry::attach: " + database_id.error().message;
        return result;
    }
    attached.id = *database_id;

    if (auto bound = attached.database->bind(user_binding()); !bound) {
        result.error = "bind(User): " + bound.error().message;
        return result;
    }
    result.ok = true;
    return result;
}

struct CreatePhaseOutcome {
    bool ok{};
    std::string error;
    std::vector<ObjectId> ids;
    std::string expected_hash;
    std::uint64_t logical_bytes{};
    std::uint64_t db_bytes_before{};   // tamanho do arquivo antes de qualquer create
    PhaseMetrics phase;                // phase.db_bytes = tamanho após o create
};

// Fase `create`: gera e persiste `params.object_count` objetos em lotes de
// `params.batch` por commit. Reaproveitada por todo workload que começa
// criando (create_only, create_delete_*, crud_full).
CreatePhaseOutcome perform_create_phase(AttachedDatabase& attached, const WorkloadParams& params,
                                        const std::filesystem::path& db_path,
                                        const std::filesystem::path& wal_path) {
    CreatePhaseOutcome outcome;

    std::error_code size_error;
    const auto initial_bytes = std::filesystem::file_size(db_path, size_error);
    outcome.db_bytes_before = size_error ? 0 : initial_bytes;
    const auto pages_read_before = attached.database->data_pages_read();

    const auto batch = params.batch == 0 ? params.object_count : params.batch;
    outcome.ids.reserve(params.object_count);
    std::ostringstream expected;
    std::vector<double> latencies_ns;
    latencies_ns.reserve(params.object_count);
    std::uint64_t errors = 0;

    // `Transaction` só tem construtor de movimento (a atribuição de movimento é
    // deletada de propósito, database.hpp:87), então trocar de transação a
    // cada lote usa `std::optional<Transaction>::emplace`, nunca `tx = ...`.
    auto first_tx = attached.database->begin();
    if (!first_tx) {
        outcome.error = "begin(create): " + first_tx.error().message;
        return outcome;
    }
    std::optional<Transaction> tx{std::move(*first_tx)};

    const auto create_start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 1; i <= params.object_count; ++i) {
        const auto generated = generate_user(params.seed, i, params.payload);
        expected << canonical_line(generated) << '\n';
        outcome.logical_bytes += logical_size_bytes(generated);

        const auto op_start = std::chrono::steady_clock::now();
        auto created_handle = attached.database->create(*tx, to_engine_user(generated));
        const auto op_end = std::chrono::steady_clock::now();
        latencies_ns.push_back(static_cast<double>(ns_between(op_start, op_end)));

        if (!created_handle) {
            ++errors;
            outcome.error = "create(User, i=" + std::to_string(i) +
                            "): " + created_handle.error().message;
            return outcome;
        }
        outcome.ids.push_back(created_handle->id());

        if (i % batch == 0 || i == params.object_count) {
            if (auto committed = tx->commit(); !committed) {
                outcome.error = "commit(create): " + committed.error().message;
                return outcome;
            }
            if (i != params.object_count) {
                auto next_tx = attached.database->begin();
                if (!next_tx) {
                    outcome.error = "begin(create, lote seguinte): " + next_tx.error().message;
                    return outcome;
                }
                tx.emplace(std::move(*next_tx));
            }
        }
    }
    const auto create_end = std::chrono::steady_clock::now();
    const auto pages_read_after = attached.database->data_pages_read();

    const auto file_bytes = std::filesystem::file_size(db_path, size_error);
    const std::uint64_t db_bytes_after = size_error ? 0 : file_bytes;
    std::error_code wal_size_error;
    const auto wal_bytes = std::filesystem::file_size(wal_path, wal_size_error);
    const auto bytes_written =
        db_bytes_after > outcome.db_bytes_before ? db_bytes_after - outcome.db_bytes_before : 0;
    const auto create_ns = ns_between(create_start, create_end);

    outcome.phase.phase = "create";
    outcome.phase.operations = params.object_count;
    outcome.phase.duration_ns = create_ns;
    outcome.phase.ops_per_second =
        create_ns > 0 ? (static_cast<double>(params.object_count) * 1'000'000'000.0) /
                            static_cast<double>(create_ns)
                     : 0.0;
    outcome.phase.bytes_per_object =
        params.object_count > 0 ? db_bytes_after / params.object_count : 0;
    outcome.phase.errors = errors;
    outcome.phase.latency_ns = percentiles_of(std::move(latencies_ns));
    outcome.phase.peak_rss_bytes = peak_rss_bytes();
    outcome.phase.db_bytes = db_bytes_after;
    outcome.phase.wal_bytes = wal_size_error ? 0 : wal_bytes;
    outcome.phase.pages_read = pages_read_after - pages_read_before;
    outcome.phase.pages_written_estimated = bytes_written / modb::storage::page_size;

    outcome.expected_hash = sha256_hex(sha256_text(expected.str()));
    outcome.ok = true;
    return outcome;
}

struct DeletePhaseOutcome {
    bool ok{};
    std::string error;
    PhaseMetrics phase;
    std::uint64_t reclaimed_bytes{};   // > 0 só se o arquivo encolheu de fato
};

// Fase `delete`: remove os objetos de `ids_in_order`, nessa ordem exata --
// quem decide a ordem (FIFO/LIFO/stride) é o chamador (§4.2).
DeletePhaseOutcome perform_delete_phase(AttachedDatabase& attached,
                                        const std::vector<ObjectId>& ids_in_order,
                                        std::uint64_t batch, const std::filesystem::path& db_path,
                                        const std::filesystem::path& wal_path) {
    DeletePhaseOutcome outcome;
    if (ids_in_order.empty()) {
        outcome.ok = true;
        outcome.phase.phase = "delete";
        return outcome;
    }

    std::error_code size_error;
    const auto before_bytes = std::filesystem::file_size(db_path, size_error);
    const std::uint64_t db_bytes_before = size_error ? 0 : before_bytes;
    const auto pages_read_before = attached.database->data_pages_read();

    const auto batch_size = batch == 0 ? ids_in_order.size() : batch;
    std::vector<double> latencies_ns;
    latencies_ns.reserve(ids_in_order.size());
    std::uint64_t errors = 0;

    auto first_tx = attached.database->begin();
    if (!first_tx) {
        outcome.error = "begin(delete): " + first_tx.error().message;
        return outcome;
    }
    std::optional<Transaction> tx{std::move(*first_tx)};

    const auto delete_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < ids_in_order.size(); ++i) {
        const auto op_start = std::chrono::steady_clock::now();
        auto removed = attached.database->remove(*tx, ids_in_order[i]);
        const auto op_end = std::chrono::steady_clock::now();
        latencies_ns.push_back(static_cast<double>(ns_between(op_start, op_end)));

        if (!removed) {
            ++errors;
            outcome.error = "remove(User, i=" + std::to_string(i) + "): " + removed.error().message;
            return outcome;
        }

        const auto position = i + 1;
        if (position % batch_size == 0 || position == ids_in_order.size()) {
            if (auto committed = tx->commit(); !committed) {
                outcome.error = "commit(delete): " + committed.error().message;
                return outcome;
            }
            if (position != ids_in_order.size()) {
                auto next_tx = attached.database->begin();
                if (!next_tx) {
                    outcome.error = "begin(delete, lote seguinte): " + next_tx.error().message;
                    return outcome;
                }
                tx.emplace(std::move(*next_tx));
            }
        }
    }
    const auto delete_end = std::chrono::steady_clock::now();
    const auto pages_read_after = attached.database->data_pages_read();

    const auto after_bytes = std::filesystem::file_size(db_path, size_error);
    const std::uint64_t db_bytes_after = size_error ? 0 : after_bytes;
    std::error_code wal_size_error;
    const auto wal_bytes = std::filesystem::file_size(wal_path, wal_size_error);
    const auto bytes_written =
        db_bytes_after > db_bytes_before ? db_bytes_after - db_bytes_before : 0;
    const auto delete_ns = ns_between(delete_start, delete_end);

    outcome.reclaimed_bytes = db_bytes_before > db_bytes_after ? db_bytes_before - db_bytes_after : 0;
    outcome.phase.phase = "delete";
    outcome.phase.operations = ids_in_order.size();
    outcome.phase.duration_ns = delete_ns;
    outcome.phase.ops_per_second =
        delete_ns > 0 ? (static_cast<double>(ids_in_order.size()) * 1'000'000'000.0) /
                            static_cast<double>(delete_ns)
                     : 0.0;
    outcome.phase.bytes_per_object = 0;   // delete não escreve conteúdo lógico por objeto
    outcome.phase.errors = errors;
    outcome.phase.latency_ns = percentiles_of(std::move(latencies_ns));
    outcome.phase.peak_rss_bytes = peak_rss_bytes();
    outcome.phase.db_bytes = db_bytes_after;
    outcome.phase.wal_bytes = wal_size_error ? 0 : wal_bytes;
    // O motor não expõe uma razão de fragmentação na API pública hoje (§4.2
    // "fragmentação registrada"). `pages_read` fica em 0 aqui mesmo em
    // create_delete_interleaved até 100k objetos -- o working set cabe
    // inteiro no buffer pool, então remove() nunca precisa de releitura
    // física; não é um proxy confiável de fragmentação neste regime.
    // O sinal real e já honesto é `duration_ns`/`ops_per_second`: medido a
    // 100k, o delete via stride (interleaved) levou ~60% mais tempo que
    // forward/reverse sequenciais na mesma semente -- é essa diferença de
    // custo que o gate (Subfase J) deve comparar entre execuções, não
    // pages_read. Um proxy de fragmentação melhor exige either um contador
    // de páginas real do motor, ou um cenário oversubscribed (Subfase Q).
    outcome.phase.pages_read = pages_read_after - pages_read_before;
    outcome.phase.pages_written_estimated = bytes_written / modb::storage::page_size;

    outcome.ok = true;
    return outcome;
}

struct DeleteVerification {
    bool all_deleted{};
    std::uint64_t still_resolving{};
};

// Validação (§9): nenhum id removido deve continuar resolvendo.
DeleteVerification verify_all_deleted(AttachedDatabase& attached,
                                      const std::vector<ObjectId>& ids) {
    DeleteVerification verification;
    for (const auto id : ids) {
        if (attached.database->get<User>(id)) {
            ++verification.still_resolving;
        }
    }
    verification.all_deleted = verification.still_resolving == 0;
    return verification;
}

std::filesystem::path make_db_path(const WorkloadParams& params, std::string_view workload_tag) {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::path{params.work_dir} /
          ("modb_load-" + std::string{workload_tag} + "-" + std::to_string(params.seed) + "-" +
           std::to_string(unique) + ".modb");
}

// Ordem de reprodução do stride já estabelecida em
// benchmarks/scenarios/object_store_lifecycle.cpp (não inventa uma nova):
// cada offset de 0..stride-1 varre o vetor inteiro àquele passo.
std::vector<ObjectId> reorder_for_delete(const std::vector<ObjectId>& ids, DeleteOrder order) {
    if (order == DeleteOrder::Forward) {
        return ids;
    }
    if (order == DeleteOrder::Reverse) {
        return std::vector<ObjectId>(ids.rbegin(), ids.rend());
    }
    constexpr std::uint64_t stride = 7;
    std::vector<ObjectId> interleaved;
    interleaved.reserve(ids.size());
    for (std::uint64_t offset = 0; offset < stride && offset < ids.size(); ++offset) {
        for (std::uint64_t i = offset; i < ids.size(); i += stride) {
            interleaved.push_back(ids[i]);
        }
    }
    return interleaved;
}

struct ReadPhaseOutcome {
    bool ok{};
    std::string error;
    PhaseMetrics phase;
    bool hash_match{};
    std::string actual_hash;
};

// Fase `read` de crud_full: relê todos os objetos criados e compara o hash
// lógico observado com `expected_hash` -- mesma validação de create_only
// (§9), agora como fase própria e cronometrada, não escondida dentro do
// create.
ReadPhaseOutcome perform_read_phase(AttachedDatabase& attached, const std::vector<ObjectId>& ids,
                                    const std::string& expected_hash, std::uint64_t object_count,
                                    const std::filesystem::path& db_path,
                                    const std::filesystem::path& wal_path) {
    ReadPhaseOutcome outcome;

    std::error_code size_error;
    const auto pages_read_before = attached.database->data_pages_read();
    std::vector<double> latencies_ns;
    latencies_ns.reserve(ids.size());
    std::ostringstream actual;
    std::uint64_t errors = 0;

    const auto read_start = std::chrono::steady_clock::now();
    for (const auto id : ids) {
        const auto op_start = std::chrono::steady_clock::now();
        auto handle = attached.database->get<User>(id);
        Result<User> value;
        if (handle) {
            value = attached.database->materialize(*handle);
        }
        const auto op_end = std::chrono::steady_clock::now();
        latencies_ns.push_back(static_cast<double>(ns_between(op_start, op_end)));

        if (!handle || !value) {
            ++errors;
            continue;
        }
        actual << canonical_line(from_engine_user(*value)) << '\n';
    }
    const auto read_end = std::chrono::steady_clock::now();
    const auto pages_read_after = attached.database->data_pages_read();

    const auto file_bytes = std::filesystem::file_size(db_path, size_error);
    const std::uint64_t db_bytes = size_error ? 0 : file_bytes;
    std::error_code wal_size_error;
    const auto wal_bytes = std::filesystem::file_size(wal_path, wal_size_error);
    const auto read_ns = ns_between(read_start, read_end);

    outcome.phase.phase = "read";
    outcome.phase.operations = ids.size();
    outcome.phase.duration_ns = read_ns;
    outcome.phase.ops_per_second =
        read_ns > 0 ? (static_cast<double>(ids.size()) * 1'000'000'000.0) /
                          static_cast<double>(read_ns)
                   : 0.0;
    outcome.phase.bytes_per_object = object_count > 0 ? db_bytes / object_count : 0;
    outcome.phase.errors = errors;
    outcome.phase.latency_ns = percentiles_of(std::move(latencies_ns));
    outcome.phase.peak_rss_bytes = peak_rss_bytes();
    outcome.phase.db_bytes = db_bytes;
    outcome.phase.wal_bytes = wal_size_error ? 0 : wal_bytes;
    outcome.phase.pages_read = pages_read_after - pages_read_before;

    outcome.actual_hash = sha256_hex(sha256_text(actual.str()));
    outcome.hash_match = errors == 0 && ids.size() == object_count && outcome.actual_hash == expected_hash;
    outcome.ok = true;
    return outcome;
}

struct UpdatePhaseOutcome {
    bool ok{};
    std::string error;
    PhaseMetrics phase;
    std::uint64_t sample_checked{};
    std::uint64_t sample_mismatches{};
};

// Fase de update genérica (update_inplace/update_grow/update_shrink, §4.2):
// `new_value_for(index)` (1-based, mesma ordem de perform_create_phase)
// devolve o valor esperado após o update. Valida só uma AMOSTRA
// determinística campo a campo (§9 item 4), não o conjunto inteiro --
// diferente de create_only/read, que conferem tudo.
UpdatePhaseOutcome perform_update_phase(
    AttachedDatabase& attached, const std::vector<ObjectId>& ids, std::string_view phase_name,
    std::uint64_t batch, const std::filesystem::path& db_path, const std::filesystem::path& wal_path,
    const std::function<GeneratedUser(std::uint64_t)>& new_value_for, std::uint64_t sample_stride) {
    UpdatePhaseOutcome outcome;
    if (ids.empty()) {
        outcome.ok = true;
        outcome.phase.phase = std::string{phase_name};
        return outcome;
    }

    std::error_code size_error;
    const auto pages_read_before = attached.database->data_pages_read();
    const auto batch_size = batch == 0 ? ids.size() : batch;
    std::vector<double> latencies_ns;
    latencies_ns.reserve(ids.size());
    std::uint64_t errors = 0;

    auto first_tx = attached.database->begin();
    if (!first_tx) {
        outcome.error = std::string{phase_name} + ": begin: " + first_tx.error().message;
        return outcome;
    }
    std::optional<Transaction> tx{std::move(*first_tx)};

    const auto phase_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto index = static_cast<std::uint64_t>(i) + 1;
        const auto new_value = new_value_for(index);

        const auto op_start = std::chrono::steady_clock::now();
        auto handle = attached.database->get<User>(ids[i]);
        Result<void> updated;
        if (handle) {
            updated = attached.database->update(*tx, *handle, to_engine_user(new_value));
        }
        const auto op_end = std::chrono::steady_clock::now();
        latencies_ns.push_back(static_cast<double>(ns_between(op_start, op_end)));

        if (!handle || !updated) {
            ++errors;
            outcome.error = std::string{phase_name} + "(i=" + std::to_string(index) +
                            "): " + (handle ? updated.error().message : handle.error().message);
            return outcome;
        }

        const auto position = i + 1;
        if (position % batch_size == 0 || position == ids.size()) {
            if (auto committed = tx->commit(); !committed) {
                outcome.error = std::string{phase_name} + ": commit: " + committed.error().message;
                return outcome;
            }
            if (position != ids.size()) {
                auto next_tx = attached.database->begin();
                if (!next_tx) {
                    outcome.error =
                        std::string{phase_name} + ": begin (lote seguinte): " + next_tx.error().message;
                    return outcome;
                }
                tx.emplace(std::move(*next_tx));
            }
        }
    }
    const auto phase_end = std::chrono::steady_clock::now();
    const auto pages_read_after = attached.database->data_pages_read();

    const auto file_bytes = std::filesystem::file_size(db_path, size_error);
    const std::uint64_t db_bytes = size_error ? 0 : file_bytes;
    std::error_code wal_size_error;
    const auto wal_bytes = std::filesystem::file_size(wal_path, wal_size_error);
    const auto phase_ns = ns_between(phase_start, phase_end);

    outcome.phase.phase = std::string{phase_name};
    outcome.phase.operations = ids.size();
    outcome.phase.duration_ns = phase_ns;
    outcome.phase.ops_per_second =
        phase_ns > 0 ? (static_cast<double>(ids.size()) * 1'000'000'000.0) /
                           static_cast<double>(phase_ns)
                     : 0.0;
    outcome.phase.bytes_per_object = ids.size() > 0 ? db_bytes / ids.size() : 0;
    outcome.phase.errors = errors;
    outcome.phase.latency_ns = percentiles_of(std::move(latencies_ns));
    outcome.phase.peak_rss_bytes = peak_rss_bytes();
    outcome.phase.db_bytes = db_bytes;
    outcome.phase.wal_bytes = wal_size_error ? 0 : wal_bytes;
    outcome.phase.pages_read = pages_read_after - pages_read_before;

    // Validação (§9 item 4): amostra determinística, campo a campo -- não o
    // conjunto inteiro.
    for (std::size_t i = 0; i < ids.size(); i += sample_stride) {
        const auto index = static_cast<std::uint64_t>(i) + 1;
        const auto expected_value = new_value_for(index);
        auto handle = attached.database->get<User>(ids[i]);
        if (!handle) {
            ++outcome.sample_mismatches;
            continue;
        }
        auto value = attached.database->materialize(*handle);
        if (!value) {
            ++outcome.sample_mismatches;
            continue;
        }
        ++outcome.sample_checked;
        const auto actual = from_engine_user(*value);
        if (canonical_line(actual) != canonical_line(expected_value)) {
            ++outcome.sample_mismatches;
        }
    }

    outcome.ok = true;
    return outcome;
}

} // namespace

CaseRunResult run_create_only_embedded(const WorkloadParams& params,
                                       std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, "create_only");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    AttachedDatabase attached;
    if (auto setup = setup_database(attached, db_path); !setup.ok) {
        result.status = "failed";
        result.error = setup.error;
        return result;
    }

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path);
    if (!create_outcome.ok) {
        result.status = "failed";
        result.error = create_outcome.error;
        return result;
    }
    result.phases.push_back(create_outcome.phase);

    // Validação (§9): reler todos os objetos criados e comparar o hash lógico
    // observado com o esperado pelo gerador -- não só a contagem.
    std::ostringstream actual;
    std::uint64_t read_errors = 0;
    for (const auto id : create_outcome.ids) {
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

    result.expected_hash = create_outcome.expected_hash;
    result.actual_hash = sha256_hex(sha256_text(actual.str()));
    result.hash_match = read_errors == 0 && create_outcome.ids.size() == params.object_count &&
                        result.expected_hash == result.actual_hash;

    if (!result.hash_match) {
        result.status = "failed";
        result.error = "validação de hash lógico falhou (" + std::to_string(read_errors) +
                       " erro(s) de releitura, " + std::to_string(create_outcome.ids.size()) +
                       "/" + std::to_string(params.object_count) + " objetos)";
        return result;
    }

    const auto bytes_written = create_outcome.phase.db_bytes > create_outcome.db_bytes_before
                                  ? create_outcome.phase.db_bytes - create_outcome.db_bytes_before
                                  : 0;
    result.peak_disk_bytes = create_outcome.phase.db_bytes;
    result.total_duration_ns = create_outcome.phase.duration_ns;
    result.write_amplification = create_outcome.logical_bytes > 0
                                    ? static_cast<double>(bytes_written) /
                                          static_cast<double>(create_outcome.logical_bytes)
                                    : 0.0;
    result.space_amplification = create_outcome.logical_bytes > 0
                                    ? static_cast<double>(result.peak_disk_bytes) /
                                          static_cast<double>(create_outcome.logical_bytes)
                                    : 0.0;
    result.status = "completed";
    result.ok = true;
    return result;
}

CaseRunResult run_create_delete_embedded(const WorkloadParams& params, DeleteOrder order,
                                         std::string_view workload_tag,
                                         std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, workload_tag);
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    AttachedDatabase attached;
    if (auto setup = setup_database(attached, db_path); !setup.ok) {
        result.status = "failed";
        result.error = setup.error;
        return result;
    }

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path);
    if (!create_outcome.ok) {
        result.status = "failed";
        result.error = create_outcome.error;
        return result;
    }
    result.phases.push_back(create_outcome.phase);

    const auto delete_ids = reorder_for_delete(create_outcome.ids, order);
    auto delete_outcome = perform_delete_phase(attached, delete_ids, params.batch, db_path, wal_path);
    result.phases.push_back(delete_outcome.phase);
    if (!delete_outcome.ok) {
        result.status = "failed";
        result.error = delete_outcome.error;
        return result;
    }
    result.reclaimed_bytes = delete_outcome.reclaimed_bytes;

    auto verification = verify_all_deleted(attached, create_outcome.ids);
    result.all_deleted = verification.all_deleted;
    result.still_resolving = verification.still_resolving;
    if (!verification.all_deleted) {
        result.status = "failed";
        result.error = std::to_string(verification.still_resolving) +
                       " objeto(s) ainda resolvem depois do delete (esperado: 0)";
        return result;
    }

    const auto total_bytes_written =
        (create_outcome.phase.db_bytes > create_outcome.db_bytes_before
            ? create_outcome.phase.db_bytes - create_outcome.db_bytes_before
            : 0) +
        (delete_outcome.phase.db_bytes > create_outcome.phase.db_bytes
            ? delete_outcome.phase.db_bytes - create_outcome.phase.db_bytes
            : 0);

    result.peak_disk_bytes = delete_outcome.phase.db_bytes;
    result.total_duration_ns = create_outcome.phase.duration_ns + delete_outcome.phase.duration_ns;
    result.write_amplification = create_outcome.logical_bytes > 0
                                    ? static_cast<double>(total_bytes_written) /
                                          static_cast<double>(create_outcome.logical_bytes)
                                    : 0.0;
    // space_amplification é "tamanho final / bytes lógicos vivos" (§8); com
    // tudo deletado não há bytes vivos -- 0.0 é "não computável", não um
    // valor de bloat inventado (o motor pode ou não encolher o arquivo).
    result.space_amplification = 0.0;
    result.status = "completed";
    result.ok = true;
    return result;
}

CaseRunResult run_crud_full_embedded(const WorkloadParams& params,
                                     std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, "crud_full");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    AttachedDatabase attached;
    if (auto setup = setup_database(attached, db_path); !setup.ok) {
        result.status = "failed";
        result.error = setup.error;
        return result;
    }

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path);
    if (!create_outcome.ok) {
        result.status = "failed";
        result.error = create_outcome.error;
        return result;
    }
    result.phases.push_back(create_outcome.phase);

    auto read_outcome = perform_read_phase(attached, create_outcome.ids, create_outcome.expected_hash,
                                           params.object_count, db_path, wal_path);
    result.phases.push_back(read_outcome.phase);
    if (!read_outcome.hash_match) {
        result.status = "failed";
        result.error = "read: hash lógico não confere com o esperado da criação";
        return result;
    }

    // update_inplace mantém o mesmo tamanho de filler (só o status muda --
    // não deveria mover o registro entre páginas); update_grow/update_shrink
    // mudam deliberadamente o tamanho para exercitar crescimento/encolhimento
    // (§4.2). Nenhum dos três reusa generate_user(seed,index,payload) puro,
    // porque isso regeneraria o valor IDÊNTICO ao original -- generate_user_ex
    // muda status e/ou tamanho de filler de propósito.
    const auto base_filler = filler_bytes_for_payload(params.payload);
    const auto grow_filler = base_filler * 2;
    const auto shrink_filler = std::max<std::size_t>(base_filler / 4, 8);
    const auto sample_stride = std::max<std::uint64_t>(1, params.object_count / 100);

    auto inplace_value_for = [&](std::uint64_t index) {
        const auto original = generate_user(params.seed, index, params.payload);
        return generate_user_ex(params.seed, index, base_filler, (original.status + 1) % 3);
    };
    auto grow_value_for = [&](std::uint64_t index) {
        const auto original = generate_user(params.seed, index, params.payload);
        return generate_user_ex(params.seed, index, grow_filler, (original.status + 2) % 3);
    };
    auto shrink_value_for = [&](std::uint64_t index) {
        const auto original = generate_user(params.seed, index, params.payload);
        return generate_user_ex(params.seed, index, shrink_filler, original.status);
    };

    auto inplace_outcome = perform_update_phase(attached, create_outcome.ids, "update_inplace",
                                                params.batch, db_path, wal_path, inplace_value_for,
                                                sample_stride);
    result.phases.push_back(inplace_outcome.phase);
    if (!inplace_outcome.ok) {
        result.status = "failed";
        result.error = inplace_outcome.error;
        return result;
    }
    if (inplace_outcome.sample_mismatches > 0) {
        result.status = "failed";
        result.error = "update_inplace: " + std::to_string(inplace_outcome.sample_mismatches) +
                       "/" + std::to_string(inplace_outcome.sample_checked) +
                       " amostra(s) não conferem campo a campo";
        return result;
    }

    auto grow_outcome = perform_update_phase(attached, create_outcome.ids, "update_grow",
                                             params.batch, db_path, wal_path, grow_value_for,
                                             sample_stride);
    result.phases.push_back(grow_outcome.phase);
    if (!grow_outcome.ok) {
        result.status = "failed";
        result.error = grow_outcome.error;
        return result;
    }
    if (grow_outcome.sample_mismatches > 0) {
        result.status = "failed";
        result.error = "update_grow: " + std::to_string(grow_outcome.sample_mismatches) + "/" +
                       std::to_string(grow_outcome.sample_checked) +
                       " amostra(s) não conferem campo a campo";
        return result;
    }

    auto shrink_outcome = perform_update_phase(attached, create_outcome.ids, "update_shrink",
                                               params.batch, db_path, wal_path, shrink_value_for,
                                               sample_stride);
    result.phases.push_back(shrink_outcome.phase);
    if (!shrink_outcome.ok) {
        result.status = "failed";
        result.error = shrink_outcome.error;
        return result;
    }
    if (shrink_outcome.sample_mismatches > 0) {
        result.status = "failed";
        result.error = "update_shrink: " + std::to_string(shrink_outcome.sample_mismatches) + "/" +
                       std::to_string(shrink_outcome.sample_checked) +
                       " amostra(s) não conferem campo a campo";
        return result;
    }

    auto delete_outcome =
        perform_delete_phase(attached, create_outcome.ids, params.batch, db_path, wal_path);
    result.phases.push_back(delete_outcome.phase);
    if (!delete_outcome.ok) {
        result.status = "failed";
        result.error = delete_outcome.error;
        return result;
    }
    result.reclaimed_bytes = delete_outcome.reclaimed_bytes;

    auto verification = verify_all_deleted(attached, create_outcome.ids);
    result.all_deleted = verification.all_deleted;
    result.still_resolving = verification.still_resolving;
    if (!verification.all_deleted) {
        result.status = "failed";
        result.error = std::to_string(verification.still_resolving) +
                       " objeto(s) ainda resolvem depois do delete (esperado: 0)";
        return result;
    }

    // Bytes físicos escritos em toda a vida do caso: soma dos crescimentos
    // positivos de db_bytes entre fases sucessivas (uma fase pode encolher o
    // arquivo ou mantê-lo do mesmo tamanho -- só o que cresce é "escrita").
    std::uint64_t total_bytes_written = 0;
    std::uint64_t prev_db_bytes = create_outcome.db_bytes_before;
    for (const auto& phase : result.phases) {
        if (phase.db_bytes > prev_db_bytes) {
            total_bytes_written += phase.db_bytes - prev_db_bytes;
        }
        prev_db_bytes = phase.db_bytes;
    }

    result.expected_hash = create_outcome.expected_hash;
    result.actual_hash = read_outcome.actual_hash;
    result.hash_match = read_outcome.hash_match;
    result.peak_disk_bytes = delete_outcome.phase.db_bytes;
    result.total_duration_ns = create_outcome.phase.duration_ns + read_outcome.phase.duration_ns +
                               inplace_outcome.phase.duration_ns + grow_outcome.phase.duration_ns +
                               shrink_outcome.phase.duration_ns + delete_outcome.phase.duration_ns;
    result.write_amplification = create_outcome.logical_bytes > 0
                                    ? static_cast<double>(total_bytes_written) /
                                          static_cast<double>(create_outcome.logical_bytes)
                                    : 0.0;
    // Tudo deletado ao final -- 0 bytes lógicos vivos, então
    // space_amplification (bytes/byte-vivo) não é computável (§8), não 0
    // inventado de bloat.
    result.space_amplification = 0.0;
    result.status = "completed";
    result.ok = true;
    return result;
}

} // namespace modb::loadtest
