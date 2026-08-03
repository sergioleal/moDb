#include "target_embedded.hpp"

#include "dataset_user.hpp"
#include "process_metrics.hpp"
#include "runner/json_util.hpp"
#include "runner/sha256.hpp"
#include "user_type.hpp"

#include "modb/object/blob_store.hpp"
#include "modb/object/database.hpp"
#include "modb/object/ref.hpp"
#include "modb/query/planner.hpp"
#include "modb/storage/buffer_pool.hpp"
#include "modb/storage/page.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace modb;
using namespace modb::object;
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

// Bytes lógicos de um GeneratedUser: os campos como o chamador os vê, não
// como o motor os codifica em página (usado para write/space amplification,
// §8) -- deliberadamente diferente de `canonical_line`, que serve só ao hash
// e carrega separadores decorativos que inflariam a contagem.
std::uint64_t logical_size_bytes(const GeneratedUser& u) {
    return sizeof(u.id) + u.login.size() + u.email.size() + u.display_name.size() +
          sizeof(u.created_at) + sizeof(u.status) + u.filler.size();
}

// Amostrador Zipf sobre ranks 1..n (§4.2.1 `read_hotspot`: "leitura
// enviesada sobre working set fixo"). CDF pré-computada -- O(n) memória,
// O(log n) por amostra -- suficiente para as escalas deste workload (até
// 1M). `s` é o expoente de inclinação usual (1.0 = Zipf clássico).
class ZipfSampler {
public:
    ZipfSampler(std::uint64_t n, double s, std::uint64_t seed) : rng_(seed) {
        cumulative_.reserve(n);
        double sum = 0.0;
        for (std::uint64_t k = 1; k <= n; ++k) {
            sum += 1.0 / std::pow(static_cast<double>(k), s);
            cumulative_.push_back(sum);
        }
        total_ = sum;
    }

    // Rank 1-based (mesma numeração de `generate_user`'s `index`).
    std::uint64_t next() {
        std::uniform_real_distribution<double> dist(0.0, total_);
        const double target = dist(rng_);
        auto it = std::lower_bound(cumulative_.begin(), cumulative_.end(), target);
        return static_cast<std::uint64_t>(it - cumulative_.begin()) + 1;
    }

private:
    std::vector<double> cumulative_;
    double total_{};
    std::mt19937_64 rng_;
};

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

// Fecha uma ProgressWindow a cada `interval` de tempo decorrido (§12) e
// repassa para `callback` (nulo = não emite nada -- comportamento anterior
// a esta subfase, sem custo além de checar um ponteiro de função a cada
// operação). Mantida por fase, nunca compartilhada entre fases.
class WindowTracker {
public:
    WindowTracker(std::string phase, ProgressCallback callback, std::chrono::nanoseconds interval,
                 std::function<std::uint64_t()> db_bytes_getter)
        : phase_(std::move(phase)), callback_(std::move(callback)), interval_(interval),
          db_bytes_getter_(std::move(db_bytes_getter)) {
        // O tracker tem exatamente o tempo de vida da fase, então é aqui que os
        // contadores de estágio zeram. As fases são sequenciais: nenhuma outra
        // corre enquanto esta acumula.
        diag::stage_reset();
    }

    void record_operation(double latency_ns) {
        const auto now = std::chrono::steady_clock::now();
        if (window_start_ == std::chrono::steady_clock::time_point{}) {
            window_start_ = now;   // primeira operação da fase abre a 1a janela
        }
        ++ops_in_window_;
        latencies_in_window_.push_back(latency_ns);
        rss_.sample();
        if (now - window_start_ >= interval_) {
            emit(now);
        }
    }

    // Pico de RSS da fase, amostrado ao longo dela -- não o pico monotônico do
    // processo (docs-process/PLANO_PROFILING.md §3, M2).
    [[nodiscard]] std::uint64_t rss_peak_bytes() { return rss_.peak(); }

    // Tempo por estágio acumulado desde o início da fase. Tudo zero num build
    // sem MODB_ENABLE_STAGE_PROFILING.
    [[nodiscard]] diag::StageSnapshot stages() const { return diag::stage_snapshot(); }

    // Fecha a última janela parcial, se sobrou alguma operação não emitida --
    // mas só quando pelo menos uma janela completa já fechou antes (§8: fase
    // mais curta que `interval` não emite janela nenhuma). Sem o guard de
    // `window_count_ > 0`, TODA fase -- mesmo uma de 5 ms -- fecharia uma
    // "janela" só de cauda no finish(), inventando um windows_summary de uma
    // linha só para casos que nunca chegaram perto de uma janela de verdade.
    void finish() {
        if (window_count_ > 0 && ops_in_window_ > 0) {
            emit(std::chrono::steady_clock::now());
        }
    }

    [[nodiscard]] bool any_window_emitted() const { return window_count_ > 0; }
    [[nodiscard]] const ProgressWindow& first_window() const { return first_window_; }
    [[nodiscard]] const ProgressWindow& last_window() const { return last_window_; }

private:
    void emit(std::chrono::steady_clock::time_point now) {
        ProgressWindow window;
        window.phase = phase_;
        window.window_index = window_count_;
        window.operations_in_window = ops_in_window_;
        window.elapsed_ns_in_window = ns_between(window_start_, now);
        window.ops_per_second = window.elapsed_ns_in_window > 0
                                   ? (static_cast<double>(ops_in_window_) * 1'000'000'000.0) /
                                         static_cast<double>(window.elapsed_ns_in_window)
                                   : 0.0;
        std::sort(latencies_in_window_.begin(), latencies_in_window_.end());
        window.p99_ns = latencies_in_window_.empty()
                          ? 0.0
                          : percentile_sorted(latencies_in_window_, 0.99);
        // Snapshot do RSS no fechamento da janela: uma série temporal quer a
        // curva, não uma marca de água monotônica que nunca desce.
        window.peak_rss_bytes = current_rss_bytes();
        window.db_bytes = db_bytes_getter_ ? db_bytes_getter_() : 0;

        if (window_count_ == 0) {
            first_window_ = window;
        }
        last_window_ = window;
        ++window_count_;
        if (callback_) {
            callback_(window);
        }

        ops_in_window_ = 0;
        latencies_in_window_.clear();
        window_start_ = now;
    }

    std::string phase_;
    ProgressCallback callback_;
    std::chrono::nanoseconds interval_;
    std::function<std::uint64_t()> db_bytes_getter_;

    std::chrono::steady_clock::time_point window_start_{};
    std::uint64_t ops_in_window_{0};
    std::vector<double> latencies_in_window_;
    std::uint64_t window_count_{0};
    ProgressWindow first_window_;
    ProgressWindow last_window_;
    // Construído junto com o tracker, que tem exatamente o tempo de vida da
    // fase -- daí o piso ser o RSS no início dela.
    RssTracker rss_;
};

// Preenche CaseRunResult::WindowsSummary a partir da PRIMEIRA fase do caso
// que de fato fechou alguma janela -- normalmente a mais longa. Casos onde
// nenhuma fase passa de `window_interval` não têm windows (§13.3: "casos com
// janelas"), não é obrigatório em todo rollup.
void adopt_windows_if_first(CaseRunResult::WindowsSummary& summary, const WindowTracker& tracker) {
    if (summary.has_windows || !tracker.any_window_emitted()) {
        return;
    }
    const auto& first = tracker.first_window();
    const auto& last = tracker.last_window();
    summary.has_windows = true;
    summary.first_ops_per_second = first.ops_per_second;
    summary.last_ops_per_second = last.ops_per_second;
    summary.first_p99_ns = first.p99_ns;
    summary.last_p99_ns = last.p99_ns;
    const auto elapsed_minutes =
        static_cast<double>(last.elapsed_ns_in_window) / 1e9 / 60.0 *
        static_cast<double>(std::max<std::uint64_t>(1, last.window_index));
    summary.slope_ops_per_second_per_min =
        elapsed_minutes > 0 ? (last.ops_per_second - first.ops_per_second) / elapsed_minutes : 0.0;
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
SetupResult setup_database(AttachedDatabase& attached, const std::filesystem::path& db_path,
                           std::size_t cache_capacity_pages = modb::storage::page_cache_capacity) {
    SetupResult result;
    auto created = Database::create(db_path, cache_capacity_pages);
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
                                        const std::filesystem::path& wal_path,
                                        CaseRunResult::WindowsSummary& windows_summary) {
    CreatePhaseOutcome outcome;

    std::error_code size_error;
    const auto initial_bytes = std::filesystem::file_size(db_path, size_error);
    outcome.db_bytes_before = size_error ? 0 : initial_bytes;
    const auto pages_read_before = attached.database->data_pages_read();

    WindowTracker window_tracker("create", params.on_progress, params.window_interval,
                                 [&db_path]() -> std::uint64_t {
                                     std::error_code ec;
                                     const auto bytes = std::filesystem::file_size(db_path, ec);
                                     return ec ? 0 : bytes;
                                 });

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
        window_tracker.record_operation(latencies_ns.back());

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
    window_tracker.finish();
    adopt_windows_if_first(windows_summary, window_tracker);

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
    outcome.phase.peak_rss_bytes = window_tracker.rss_peak_bytes();
    outcome.phase.stages = window_tracker.stages();
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
                                        const std::filesystem::path& wal_path,
                                        const ProgressCallback& on_progress,
                                        std::chrono::nanoseconds window_interval,
                                        CaseRunResult::WindowsSummary& windows_summary) {
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

    WindowTracker window_tracker("delete", on_progress, window_interval,
                                 [&db_path]() -> std::uint64_t {
                                     std::error_code ec;
                                     const auto bytes = std::filesystem::file_size(db_path, ec);
                                     return ec ? 0 : bytes;
                                 });

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
        window_tracker.record_operation(latencies_ns.back());

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
    window_tracker.finish();
    adopt_windows_if_first(windows_summary, window_tracker);

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
    outcome.phase.peak_rss_bytes = window_tracker.rss_peak_bytes();
    outcome.phase.stages = window_tracker.stages();
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
                                    const std::filesystem::path& wal_path,
                                    const ProgressCallback& on_progress,
                                    std::chrono::nanoseconds window_interval,
                                    CaseRunResult::WindowsSummary& windows_summary) {
    ReadPhaseOutcome outcome;

    std::error_code size_error;
    const auto pages_read_before = attached.database->data_pages_read();
    std::vector<double> latencies_ns;
    latencies_ns.reserve(ids.size());
    std::ostringstream actual;
    std::uint64_t errors = 0;

    WindowTracker window_tracker("read", on_progress, window_interval,
                                 [&db_path]() -> std::uint64_t {
                                     std::error_code ec;
                                     const auto bytes = std::filesystem::file_size(db_path, ec);
                                     return ec ? 0 : bytes;
                                 });

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
        window_tracker.record_operation(latencies_ns.back());

        if (!handle || !value) {
            ++errors;
            continue;
        }
        actual << canonical_line(from_engine_user(*value)) << '\n';
    }
    const auto read_end = std::chrono::steady_clock::now();
    const auto pages_read_after = attached.database->data_pages_read();
    window_tracker.finish();
    adopt_windows_if_first(windows_summary, window_tracker);

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
    outcome.phase.peak_rss_bytes = window_tracker.rss_peak_bytes();
    outcome.phase.stages = window_tracker.stages();
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
    const std::function<GeneratedUser(std::uint64_t)>& new_value_for, std::uint64_t sample_stride,
    const ProgressCallback& on_progress, std::chrono::nanoseconds window_interval,
    CaseRunResult::WindowsSummary& windows_summary) {
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

    WindowTracker window_tracker(std::string{phase_name}, on_progress, window_interval,
                                 [&db_path]() -> std::uint64_t {
                                     std::error_code ec;
                                     const auto bytes = std::filesystem::file_size(db_path, ec);
                                     return ec ? 0 : bytes;
                                 });

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
        window_tracker.record_operation(latencies_ns.back());

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
    window_tracker.finish();
    adopt_windows_if_first(windows_summary, window_tracker);

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
    outcome.phase.peak_rss_bytes = window_tracker.rss_peak_bytes();
    outcome.phase.stages = window_tracker.stages();
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

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path, result.windows);
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

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path, result.windows);
    if (!create_outcome.ok) {
        result.status = "failed";
        result.error = create_outcome.error;
        return result;
    }
    result.phases.push_back(create_outcome.phase);

    const auto delete_ids = reorder_for_delete(create_outcome.ids, order);
    auto delete_outcome = perform_delete_phase(attached, delete_ids, params.batch, db_path, wal_path,
                                              params.on_progress, params.window_interval,
                                              result.windows);
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

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path, result.windows);
    if (!create_outcome.ok) {
        result.status = "failed";
        result.error = create_outcome.error;
        return result;
    }
    result.phases.push_back(create_outcome.phase);

    auto read_outcome = perform_read_phase(attached, create_outcome.ids, create_outcome.expected_hash,
                                           params.object_count, db_path, wal_path,
                                           params.on_progress, params.window_interval,
                                           result.windows);
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
                                                sample_stride, params.on_progress,
                                                params.window_interval, result.windows);
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
                                             sample_stride, params.on_progress,
                                             params.window_interval, result.windows);
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
                                               sample_stride, params.on_progress,
                                               params.window_interval, result.windows);
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
        perform_delete_phase(attached, create_outcome.ids, params.batch, db_path, wal_path,
                             params.on_progress, params.window_interval, result.windows);
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

CaseRunResult run_read_hotspot_embedded(const WorkloadParams& params,
                                        std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, "read_hotspot");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    AttachedDatabase attached;
    if (auto setup = setup_database(attached, db_path); !setup.ok) {
        result.status = "failed";
        result.error = setup.error;
        return result;
    }

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path, result.windows);
    if (!create_outcome.ok) {
        result.status = "failed";
        result.error = create_outcome.error;
        return result;
    }
    result.phases.push_back(create_outcome.phase);

    if (params.object_count == 0) {
        result.status = "failed";
        result.error = "read_hotspot: object_count=0 -- nada para amostrar";
        return result;
    }

    // Leituras enviesadas por Zipf sobre o working set inteiro -- 3x o
    // tamanho do working set é um múltiplo arbitrário, mas o bastante para
    // um rank "quente" (baixo) ser reamostrado muitas vezes e pressionar o
    // buffer pool de verdade, em vez de ler cada objeto uma única vez.
    const auto read_count = std::max<std::uint64_t>(params.object_count * 3, 1000);
    ZipfSampler sampler(params.object_count, /*s=*/1.0, params.seed ^ 0x5A17'F000'0000'0001ULL);

    auto& buffer_pool = attached.database->page_file().buffer_pool();
    buffer_pool.reset_metrics();

    std::vector<double> latencies_ns;
    latencies_ns.reserve(read_count);
    std::ostringstream expected_stream, actual_stream;
    std::uint64_t errors = 0;

    RssTracker rss;
    const auto read_start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < read_count; ++i) {
        const auto rank = sampler.next();
        const auto expected_user = generate_user(params.seed, rank, params.payload);
        expected_stream << canonical_line(expected_user) << '\n';

        const auto op_start = std::chrono::steady_clock::now();
        auto handle = attached.database->get<User>(create_outcome.ids[rank - 1]);
        Result<User> value;
        if (handle) {
            value = attached.database->materialize(*handle);
        }
        const auto op_end = std::chrono::steady_clock::now();
        latencies_ns.push_back(static_cast<double>(ns_between(op_start, op_end)));
        rss.sample();

        if (!handle || !value) {
            ++errors;
            continue;
        }
        actual_stream << canonical_line(from_engine_user(*value)) << '\n';
    }
    const auto read_end = std::chrono::steady_clock::now();
    const auto metrics = buffer_pool.metrics();
    const auto total_accesses = metrics.hits + metrics.misses;
    const auto hit_rate = total_accesses > 0
                             ? static_cast<double>(metrics.hits) / static_cast<double>(total_accesses)
                             : -1.0;

    std::error_code size_error;
    const auto db_bytes = std::filesystem::file_size(db_path, size_error);
    std::error_code wal_size_error;
    const auto wal_bytes = std::filesystem::file_size(wal_path, wal_size_error);
    const auto read_ns = ns_between(read_start, read_end);

    PhaseMetrics phase;
    phase.phase = "read_hotspot";
    phase.operations = read_count;
    phase.duration_ns = read_ns;
    phase.ops_per_second = read_ns > 0 ? (static_cast<double>(read_count) * 1'000'000'000.0) /
                                            static_cast<double>(read_ns)
                                       : 0.0;
    phase.errors = errors;
    phase.latency_ns = percentiles_of(std::move(latencies_ns));
    phase.peak_rss_bytes = rss.peak();
    phase.db_bytes = size_error ? 0 : db_bytes;
    phase.wal_bytes = wal_size_error ? 0 : wal_bytes;
    phase.cache_hit_rate = hit_rate;
    result.phases.push_back(phase);

    result.expected_hash = sha256_hex(sha256_text(expected_stream.str()));
    result.actual_hash = sha256_hex(sha256_text(actual_stream.str()));
    result.hash_match = errors == 0 && result.expected_hash == result.actual_hash;
    if (!result.hash_match) {
        result.status = "failed";
        result.error = "read_hotspot: valores lidos não conferem com o esperado (" +
                       std::to_string(errors) + " erro(s) de releitura)";
        return result;
    }

    result.peak_disk_bytes = phase.db_bytes;
    result.total_duration_ns = create_outcome.phase.duration_ns + read_ns;
    // read_hotspot só lê -- `write_amplification` (convenção compartilhada
    // com create_only/crud_full/oversubscribed_churn, §8) é bytes escritos
    // DURANTE a fase medida, não o tamanho total do arquivo (isso é
    // `space_amplification`). Um valor perto de 0 aqui é o sinal correto
    // (nenhuma escrita nova nesta fase), não um bug.
    const auto bytes_written_during_read = phase.db_bytes > create_outcome.phase.db_bytes
                                              ? phase.db_bytes - create_outcome.phase.db_bytes
                                              : 0;
    result.write_amplification = create_outcome.logical_bytes > 0
                                    ? static_cast<double>(bytes_written_during_read) /
                                          static_cast<double>(create_outcome.logical_bytes)
                                    : 0.0;
    result.space_amplification = create_outcome.logical_bytes > 0
                                    ? static_cast<double>(phase.db_bytes) /
                                          static_cast<double>(create_outcome.logical_bytes)
                                    : 0.0;
    result.status = "completed";
    result.ok = true;
    return result;
}

CaseRunResult run_range_scan_sweep_embedded(const WorkloadParams& params,
                                            std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, "range_scan_sweep");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    AttachedDatabase attached;
    if (auto setup = setup_database(attached, db_path); !setup.ok) {
        result.status = "failed";
        result.error = setup.error;
        return result;
    }

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path, result.windows);
    if (!create_outcome.ok) {
        result.status = "failed";
        result.error = create_outcome.error;
        return result;
    }
    result.phases.push_back(create_outcome.phase);

    constexpr FieldId kIdField{1};
    if (auto indexed = attached.database->create_index<User>(kIdField); !indexed) {
        result.status = "failed";
        result.error = "create_index<User>(id): " + indexed.error().message;
        return result;
    }

    // §4.2.1: seletividade de 0,01% a 100% do working set -- cada uma vira
    // uma fase própria (o nome já registra se o plano usou índice ou table
    // scan, §16 "plano registrado").
    const struct { const char* label; double selectivity; } sweeps[] = {
        {"0.01pct", 0.0001}, {"0.1pct", 0.001}, {"1pct", 0.01},
        {"10pct", 0.10},     {"100pct", 1.0},
    };

    for (const auto& sweep : sweeps) {
        const auto count = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(std::llround(static_cast<double>(params.object_count) *
                                                       sweep.selectivity)));
        const std::uint64_t lo = 1;
        const std::uint64_t hi = std::min(params.object_count, lo + count - 1);
        const auto expected_count = hi - lo + 1;

        auto query = attached.database->query<User>().between(
            kIdField, static_cast<std::int64_t>(lo), static_cast<std::int64_t>(hi));
        const auto plan = query.plan();

        std::uint64_t actual_count = 0;
        std::uint64_t errors = 0;
        RssTracker rss;   // uma fase por seletividade, um tracker por fase
        const auto scan_start = std::chrono::steady_clock::now();
        for (auto& row : std::move(query).stream()) {
            if (row) {
                ++actual_count;
            } else {
                ++errors;
            }
            rss.sample();
        }
        const auto scan_end = std::chrono::steady_clock::now();

        if (actual_count != expected_count || errors != 0) {
            result.status = "failed";
            result.error = "range_scan_sweep(" + std::string(sweep.label) +
                           "): contagem esperada=" + std::to_string(expected_count) +
                           " obtida=" + std::to_string(actual_count) +
                           " erros=" + std::to_string(errors);
            return result;
        }

        PhaseMetrics phase;
        phase.phase = std::string("scan_") + sweep.label + "_" +
                     query::access_name(plan.access);
        phase.operations = actual_count;
        const auto scan_ns = ns_between(scan_start, scan_end);
        phase.duration_ns = scan_ns;
        phase.ops_per_second = scan_ns > 0 ? (static_cast<double>(actual_count) * 1'000'000'000.0) /
                                                static_cast<double>(scan_ns)
                                           : 0.0;
        phase.peak_rss_bytes = rss.peak();
        result.phases.push_back(phase);
    }

    result.status = "completed";
    result.ok = true;
    result.total_duration_ns = 0;
    for (const auto& phase : result.phases) {
        result.total_duration_ns += phase.duration_ns;
    }
    result.peak_disk_bytes = create_outcome.phase.db_bytes;
    result.hash_match = true;   // não há hash lógico separado -- a invariante é a contagem por fase
    return result;
}

namespace {

// Estado compartilhado do mixed_oltp (§4.2.1): tudo protegido por `mutex` --
// o motor é single-thread (ADR-011), então mesmo a contabilidade em memória
// (não só as chamadas ao `Database`) precisa ficar atrás do mesmo lock.
struct MixedOltpSharedState {
    std::mutex mutex;
    std::vector<ObjectId> live_ids;
    std::unordered_map<std::uint64_t, std::uint64_t> logical_id_of;   // ObjectId.value -> logical id
    std::unordered_map<std::uint64_t, GeneratedUser> expected;         // logical id -> valor esperado atual
    std::uint64_t next_logical_id{0};
    std::uint64_t created{0};
    std::uint64_t deleted{0};
    std::uint64_t errors{0};
    std::vector<double> latencies_ns;
    // Mix alcançado + estágios atribuídos por classe (PLANO_PROFILER.md
    // §3.2/§4.4). Protegido pelo mesmo `mutex`, como todo o resto.
    OperationClassBreakdown by_class{};
};

// Uma sessão do mixed_oltp: `ops` operações na razão `reads_per_write`
// (PLANO_PROFILER.md §4.2). Cada operação inteira -- begin, engine, commit,
// contabilidade -- roda sob `state.mutex`: contenção real na fila de entrada do
// motor single-thread, não paralelismo real dentro dele.
void mixed_oltp_worker(AttachedDatabase& attached, MixedOltpSharedState& state, std::uint64_t ops,
                       std::uint64_t base_seed, std::uint64_t rng_seed, std::string payload,
                       std::uint64_t reads_per_write) {
    std::mt19937_64 rng(rng_seed);
    std::uniform_int_distribution<int> status_dist(0, 2);
    const auto filler_bytes = filler_bytes_for_payload(payload);

    for (std::uint64_t i = 0; i < ops; ++i) {
        // Seleção por inteiro, não por limiar em ponto flutuante (§4.2): assim
        // as frações são racionais exatas, e reads_per_write=0 significa "só
        // escrita" -- um valor legítimo, não um caso degenerado.
        OperationClass intended;
        if (rng() % (reads_per_write + 1) != 0) {
            intended = OperationClass::read;
        } else {
            // Composição interna das escritas preservada: os antigos 5/10/5 são
            // 25%/50%/25% das escritas. Mantida como estava para que mudar a
            // razão mude UMA variável.
            const auto pick = rng() % 4;
            intended = pick == 0   ? OperationClass::create
                       : pick == 3 ? OperationClass::remove
                                   : OperationClass::update;
        }
        const auto op_start = std::chrono::steady_clock::now();
        // A operação inteira roda dentro de try/catch: qualquer exceção
        // (ex.: um `.at()` sem chave por um bug de invariante) escapando de
        // uma thread de worker chamaria std::terminate no processo inteiro
        // em vez de só falhar o caso -- pior que qualquer erro que a própria
        // lógica já trata. `lock` já foi liberado pelo unwind quando o catch
        // roda, então reacquire só para contabilizar com segurança.
        try {
        std::lock_guard<std::mutex> lock(state.mutex);

        // Uma classe pretendida que não pode ser executada vira `noop` e é
        // contada como tal (§4.4): antes, read/update/delete com o conjunto vivo
        // vazio não faziam nada e ainda contavam como operação executada, o que
        // tornava a razão alcançada invisível.
        const auto actual = (intended != OperationClass::create && state.live_ids.empty())
                                ? OperationClass::noop
                                : intended;
        // Atribuição por classe pela diferença de dois snapshots (§3.2): custo
        // zero dentro do motor, e correto sem sincronização nova porque a
        // operação inteira roda sob este lock.
        const auto stages_before = diag::stage_snapshot();

        if (actual == OperationClass::create) {
            const auto logical_id = ++state.next_logical_id;
            const auto generated = generate_user(base_seed, logical_id, payload);
            auto tx = attached.database->begin();
            if (!tx) {
                ++state.errors;
            } else {
                auto created = attached.database->create(*tx, to_engine_user(generated));
                if (!created) {
                    ++state.errors;
                } else if (auto committed = tx->commit(); !committed) {
                    ++state.errors;
                } else {
                    state.live_ids.push_back(created->id());
                    state.logical_id_of[created->id().value] = logical_id;
                    state.expected[logical_id] = generated;
                    ++state.created;
                }
            }
        } else if (actual == OperationClass::read) {
            const auto idx = rng() % state.live_ids.size();
            auto handle = attached.database->get<User>(state.live_ids[idx]);
            if (!handle) {
                ++state.errors;
            } else if (auto value = attached.database->materialize(*handle); !value) {
                ++state.errors;
            }
        } else if (actual == OperationClass::update) {
            const auto idx = rng() % state.live_ids.size();
            const auto object_id = state.live_ids[idx];
            const auto logical_id = state.logical_id_of.at(object_id.value);
            const auto new_value =
                generate_user_ex(base_seed, logical_id, filler_bytes, status_dist(rng));
            auto handle = attached.database->get<User>(object_id);
            if (!handle) {
                ++state.errors;
            } else {
                auto tx = attached.database->begin();
                if (!tx) {
                    ++state.errors;
                } else if (auto updated = attached.database->update(*tx, *handle,
                                                                    to_engine_user(new_value));
                          !updated) {
                    ++state.errors;
                } else if (auto committed = tx->commit(); !committed) {
                    ++state.errors;
                } else {
                    state.expected[logical_id] = new_value;
                }
            }
        } else if (actual == OperationClass::remove) {
            const auto idx = rng() % state.live_ids.size();
            const auto object_id = state.live_ids[idx];
            auto tx = attached.database->begin();
            if (!tx) {
                ++state.errors;
            } else if (auto removed = attached.database->remove(*tx, object_id); !removed) {
                ++state.errors;
            } else if (auto committed = tx->commit(); !committed) {
                ++state.errors;
            } else {
                const auto logical_id = state.logical_id_of.at(object_id.value);
                state.expected.erase(logical_id);
                state.logical_id_of.erase(object_id.value);
                state.live_ids[idx] = state.live_ids.back();
                state.live_ids.pop_back();
                ++state.deleted;
            }
        }

        auto& bucket = state.by_class[static_cast<std::size_t>(actual)];
        ++bucket.operations;
        diag::stage_accumulate_delta(bucket.stages, stages_before, diag::stage_snapshot());

        const auto op_end = std::chrono::steady_clock::now();
        state.latencies_ns.push_back(static_cast<double>(ns_between(op_start, op_end)));
        } catch (...) {
            std::lock_guard<std::mutex> lock(state.mutex);
            ++state.errors;
        }
    }
}

} // namespace

CaseRunResult run_mixed_oltp_embedded(const WorkloadParams& params,
                                      std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, "mixed_oltp");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    AttachedDatabase attached;
    if (auto setup = setup_database(attached, db_path); !setup.ok) {
        result.status = "failed";
        result.error = setup.error;
        return result;
    }

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path, result.windows);
    if (!create_outcome.ok) {
        result.status = "failed";
        result.error = create_outcome.error;
        return result;
    }
    result.phases.push_back(create_outcome.phase);

    MixedOltpSharedState state;
    state.next_logical_id = params.object_count;
    state.live_ids = create_outcome.ids;
    for (std::uint64_t i = 0; i < create_outcome.ids.size(); ++i) {
        const auto logical_id = i + 1;
        state.logical_id_of[create_outcome.ids[i].value] = logical_id;
        state.expected[logical_id] = generate_user(params.seed, logical_id, params.payload);
    }
    state.created = params.object_count;

    const auto concurrency = std::max<std::uint64_t>(1, params.concurrency);
    // Volume arbitrário mas generoso o bastante pras threads de fato
    // competirem pelo mutex em vez de terminarem antes de se cruzarem.
    const auto total_ops = std::max<std::uint64_t>(params.object_count * 5, 1000);
    const auto ops_per_thread = total_ops / concurrency;

    // Sessões concorrentes: amostrar por operação exigiria um tracker
    // compartilhado entre threads. Aqui o RSS cresce monotonicamente (a
    // contabilidade em memória só acumula), então limitar por início/fim
    // captura o pico sem sincronização.
    RssTracker rss;
    // A fase de create já terminou (o WindowTracker dela foi destruído), então
    // os contadores de estágio zeram aqui para medir só a fase de mix. Esta fase
    // não usa WindowTracker -- é multithread -- então o reset é explícito.
    diag::stage_reset();
    const auto mixed_start = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> threads;
        threads.reserve(concurrency);
        for (std::uint64_t t = 0; t < concurrency; ++t) {
            const auto rng_seed = params.seed ^ (0x9E37'79B9'7F4A'7C15ULL * (t + 1));
            threads.emplace_back(mixed_oltp_worker, std::ref(attached), std::ref(state),
                                 ops_per_thread, params.seed, rng_seed, params.payload,
                                 params.reads_per_write);
        }
        for (auto& th : threads) {
            th.join();
        }
    }
    const auto mixed_end = std::chrono::steady_clock::now();
    const auto mixed_stages = diag::stage_snapshot();

    // Reconciliação independente (§4.2.1 "contagem final reconcilia"): conta
    // de verdade quantos `User` existem no banco -- não só confia na
    // contabilidade em memória, que poderia estar certa por acidente mesmo
    // com um bug real na sincronização.
    std::uint64_t engine_count = 0;
    for (auto& row : attached.database->query<User>().stream()) {
        if (row) {
            ++engine_count;
        }
    }
    const auto expected_live = state.created - state.deleted;
    const auto bookkeeping_count = state.live_ids.size();
    const bool reconciled = engine_count == expected_live && bookkeeping_count == expected_live;

    // Checksum de amostra determinística (§4.2.1): ordena os logical_ids
    // sobreviventes e amostra por passo fixo -- mesma disciplina do
    // `sample_stride` de crud_full, não uma amostra aleatória a cada run.
    std::vector<std::uint64_t> surviving;
    surviving.reserve(state.expected.size());
    for (const auto& [logical_id, unused] : state.expected) {
        (void)unused;
        surviving.push_back(logical_id);
    }
    std::sort(surviving.begin(), surviving.end());
    std::unordered_map<std::uint64_t, ObjectId> object_id_of_logical;
    object_id_of_logical.reserve(state.logical_id_of.size());
    for (const auto& [object_value, logical_id] : state.logical_id_of) {
        object_id_of_logical[logical_id] = ObjectId{object_value};
    }

    const auto sample_stride = std::max<std::uint64_t>(1, surviving.size() / 100);
    std::ostringstream expected_stream, actual_stream;
    std::uint64_t sample_checked = 0, sample_mismatches = 0;
    for (std::size_t i = 0; i < surviving.size(); i += sample_stride) {
        const auto logical_id = surviving[i];
        expected_stream << canonical_line(state.expected.at(logical_id)) << '\n';
        ++sample_checked;
        auto object_it = object_id_of_logical.find(logical_id);
        if (object_it == object_id_of_logical.end()) {
            ++sample_mismatches;
            continue;
        }
        auto handle = attached.database->get<User>(object_it->second);
        Result<User> value;
        if (handle) {
            value = attached.database->materialize(*handle);
        }
        if (!handle || !value) {
            ++sample_mismatches;
            continue;
        }
        const auto actual_line = canonical_line(from_engine_user(*value));
        actual_stream << actual_line << '\n';
        if (actual_line != canonical_line(state.expected.at(logical_id))) {
            ++sample_mismatches;
        }
    }

    result.expected_hash = sha256_hex(sha256_text(expected_stream.str()));
    result.actual_hash = sha256_hex(sha256_text(actual_stream.str()));
    result.hash_match = reconciled && sample_mismatches == 0;

    const auto mixed_ns = ns_between(mixed_start, mixed_end);
    PhaseMetrics phase;
    phase.phase = "mixed_oltp";
    phase.operations = ops_per_thread * concurrency;
    phase.duration_ns = mixed_ns;
    phase.ops_per_second = mixed_ns > 0 ? (static_cast<double>(phase.operations) * 1'000'000'000.0) /
                                            static_cast<double>(mixed_ns)
                                       : 0.0;
    phase.errors = state.errors;
    phase.latency_ns = percentiles_of(std::move(state.latencies_ns));
    phase.peak_rss_bytes = rss.peak();
    phase.stages = mixed_stages;
    // Mix alcançado e estágios por classe (§3.2/§4.4). Este é o único workload
    // de fase única com mix, e portanto o único que preenche o bloco.
    phase.has_operation_classes = true;
    phase.operation_classes = state.by_class;
    phase.reads_per_write_configured = params.reads_per_write;
    std::error_code size_error;
    const auto db_bytes = std::filesystem::file_size(db_path, size_error);
    phase.db_bytes = size_error ? 0 : db_bytes;
    std::error_code wal_error;
    const auto wal_bytes = std::filesystem::file_size(wal_path, wal_error);
    phase.wal_bytes = wal_error ? 0 : wal_bytes;
    result.phases.push_back(phase);

    if (!result.hash_match) {
        result.status = "failed";
        std::ostringstream err;
        if (!reconciled) {
            err << "mixed_oltp: contagem não reconcilia (motor=" << engine_count
                << " contabilidade=" << bookkeeping_count << " esperado=" << expected_live << "). ";
        }
        if (sample_mismatches > 0) {
            err << "mixed_oltp: " << sample_mismatches << "/" << sample_checked
                << " amostra(s) não conferem.";
        }
        result.error = err.str();
        return result;
    }

    result.total_duration_ns = create_outcome.phase.duration_ns + mixed_ns;
    result.peak_disk_bytes = phase.db_bytes;
    // Sob criação/atualização/remoção concorrentes intercaladas não há um
    // denominador "bytes lógicos gerados" limpo como nos outros workloads --
    // 0.0 é "não computado", não um valor inventado (mesma convenção de
    // `create_delete_embedded`'s space_amplification após tudo deletado).
    result.write_amplification = 0.0;
    result.space_amplification = 0.0;
    result.status = "completed";
    result.ok = true;
    return result;
}

CaseRunResult run_snapshot_hold_embedded(const WorkloadParams& params,
                                         std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, "snapshot_hold");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    AttachedDatabase attached;
    if (auto setup = setup_database(attached, db_path); !setup.ok) {
        result.status = "failed";
        result.error = setup.error;
        return result;
    }

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path, result.windows);
    if (!create_outcome.ok) {
        result.status = "failed";
        result.error = create_outcome.error;
        return result;
    }
    result.phases.push_back(create_outcome.phase);

    auto opened = attached.database->snapshot();
    if (!opened) {
        result.status = "failed";
        result.error = "database.snapshot(): " + opened.error().message;
        return result;
    }
    std::optional<Snapshot> held{std::move(*opened)};

    // Leitura pela snapshot ANTES do churn -- "o estado da abertura" que
    // precisa continuar idêntico até o fechamento (§4.2.1).
    std::ostringstream baseline_stream;
    for (const auto id : create_outcome.ids) {
        auto value = attached.database->get<User>(id, *held);
        if (!value) {
            result.status = "failed";
            result.error = "leitura inicial pela snapshot falhou: " + value.error().message;
            return result;
        }
        baseline_stream << canonical_line(from_engine_user(*value)) << '\n';
    }
    const auto baseline_hash = sha256_hex(sha256_text(baseline_stream.str()));

    // Churn: cada id tocado no máximo uma vez (update OU delete, nunca os
    // dois) -- uma segunda alteração no mesmo objeto com a snapshot ainda
    // aberta falharia com snapshot_conflict (só uma versão `previous` cabe).
    const auto n = create_outcome.ids.size();
    const auto update_end = n / 3;
    const auto delete_end = (2 * n) / 3;

    std::unordered_map<std::uint64_t, GeneratedUser> final_expected;   // logical id -> valor final
    const auto filler_bytes = filler_bytes_for_payload(params.payload);
    std::uint64_t churn_errors = 0;

    RssTracker rss;
    const auto hold_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        rss.sample();   // retenção MVCC é justamente onde o RSS por fase importa
        const auto logical_id = i + 1;
        if (i < update_end) {
            const auto new_value = generate_user_ex(params.seed, logical_id, filler_bytes,
                                                     static_cast<std::int32_t>((logical_id + 1) % 3));
            auto handle = attached.database->get<User>(create_outcome.ids[i]);
            if (!handle) {
                ++churn_errors;
                continue;
            }
            auto tx = attached.database->begin();
            if (!tx || !attached.database->update(*tx, *handle, to_engine_user(new_value)) ||
                !tx->commit()) {
                ++churn_errors;
                continue;
            }
            final_expected[logical_id] = new_value;
        } else if (i < delete_end) {
            auto tx = attached.database->begin();
            if (!tx || !attached.database->remove(*tx, create_outcome.ids[i]) || !tx->commit()) {
                ++churn_errors;
                continue;
            }
            // removido -- não entra em final_expected.
        } else {
            final_expected[logical_id] = generate_user(params.seed, logical_id, params.payload);
        }
    }
    // Objetos novos, fora do working set original -- não existiam na época
    // da snapshot, então nunca deveriam aparecer numa releitura por ela.
    const auto extra_count = std::max<std::uint64_t>(1, n / 10);
    for (std::uint64_t i = 0; i < extra_count; ++i) {
        auto tx = attached.database->begin();
        if (!tx) {
            ++churn_errors;
            continue;
        }
        const auto generated = generate_user(params.seed ^ 0x5AFE'0001ULL, n + i + 1, params.payload);
        if (!attached.database->create(*tx, to_engine_user(generated)) || !tx->commit()) {
            ++churn_errors;
        }
    }

    if (churn_errors > 0) {
        result.status = "failed";
        result.error = "snapshot_hold: " + std::to_string(churn_errors) + " erro(s) durante o churn";
        return result;
    }

    // Baseline de registros vivos "esperados" agora: sobreviventes do working
    // set original (n menos os deletados) MAIS os `extra_count` objetos
    // criados fora dele -- sem somar `extra_count` aqui, cada um deles seria
    // contado como se fosse uma versão antiga retida pela snapshot aberta,
    // inflando `retained_versions` por exatamente esse tanto.
    const auto live_baseline = (n - (delete_end - update_end)) + extra_count;
    const auto retained_while_open =
        attached.database->data_record_count() > live_baseline
            ? attached.database->data_record_count() - live_baseline
            : 0;

    // Releitura pela MESMA snapshot ainda aberta -- deve bater byte a byte
    // com a leitura antes do churn (isolamento MVCC de verdade, não simulado).
    std::ostringstream after_churn_stream;
    for (const auto id : create_outcome.ids) {
        auto value = attached.database->get<User>(id, *held);
        if (!value) {
            result.status = "failed";
            result.error = "releitura pela snapshot (ainda aberta) falhou: " + value.error().message;
            return result;
        }
        after_churn_stream << canonical_line(from_engine_user(*value)) << '\n';
    }
    const auto after_churn_hash = sha256_hex(sha256_text(after_churn_stream.str()));
    const bool snapshot_stable = after_churn_hash == baseline_hash;

    held.reset();   // fecha a snapshot -- libera a época para o GC.
    const auto hold_end = std::chrono::steady_clock::now();

    auto gc = attached.database->collect_garbage();
    if (!gc) {
        result.status = "failed";
        result.error = "snapshot_hold: collect_garbage() após fechar a snapshot: " + gc.error().message;
        return result;
    }
    const auto reclaimed = *gc;

    // Depois de fechar: uma leitura NORMAL (sem snapshot) deve refletir o
    // churn -- prova que a snapshot não vazou isolamento além do seu escopo.
    std::uint64_t still_resolving_deleted = 0;
    std::ostringstream final_actual_stream;
    std::vector<std::uint64_t> surviving_ids;
    surviving_ids.reserve(final_expected.size());
    for (const auto& [logical_id, unused] : final_expected) {
        (void)unused;
        surviving_ids.push_back(logical_id);
    }
    std::sort(surviving_ids.begin(), surviving_ids.end());
    std::ostringstream final_expected_stream;
    for (const auto logical_id : surviving_ids) {
        final_expected_stream << canonical_line(final_expected.at(logical_id)) << '\n';
        auto handle = attached.database->get<User>(create_outcome.ids[logical_id - 1]);
        Result<User> value;
        if (handle) {
            value = attached.database->materialize(*handle);
        }
        if (!handle || !value) {
            result.status = "failed";
            result.error = "leitura pós-fechamento falhou para o id lógico " +
                           std::to_string(logical_id);
            return result;
        }
        final_actual_stream << canonical_line(from_engine_user(*value)) << '\n';
    }
    for (std::size_t i = update_end; i < delete_end; ++i) {
        if (attached.database->get<User>(create_outcome.ids[i])) {
            ++still_resolving_deleted;
        }
    }

    const auto final_expected_hash = sha256_hex(sha256_text(final_expected_stream.str()));
    const auto final_actual_hash = sha256_hex(sha256_text(final_actual_stream.str()));

    result.all_deleted = still_resolving_deleted == 0;
    result.still_resolving = still_resolving_deleted;
    result.hash_match =
        snapshot_stable && result.all_deleted && final_expected_hash == final_actual_hash;
    result.expected_hash = final_expected_hash;
    result.actual_hash = final_actual_hash;

    if (!result.hash_match) {
        result.status = "failed";
        std::ostringstream err;
        if (!snapshot_stable) {
            err << "snapshot_hold: a leitura pela snapshot mudou durante o churn (isolamento MVCC "
                  "violado). ";
        }
        if (!result.all_deleted) {
            err << still_resolving_deleted << " objeto(s) removido(s) durante o churn ainda "
                                              "resolvem depois do fechamento. ";
        }
        if (final_expected_hash != final_actual_hash) {
            err << "estado final não confere com o esperado pós-churn.";
        }
        result.error = err.str();
        return result;
    }

    std::error_code size_error;
    const auto db_bytes = std::filesystem::file_size(db_path, size_error);
    PhaseMetrics phase;
    phase.phase = "hold";
    phase.operations = n;
    phase.duration_ns = ns_between(hold_start, hold_end);
    phase.peak_rss_bytes = rss.peak();
    phase.db_bytes = size_error ? 0 : db_bytes;
    phase.retained_versions = retained_while_open;
    phase.errors = churn_errors;
    result.phases.push_back(phase);

    result.total_duration_ns = create_outcome.phase.duration_ns + phase.duration_ns;
    result.peak_disk_bytes = phase.db_bytes;
    // `collect_garbage()` devolve uma CONTAGEM de registros recuperados, não
    // bytes -- `CaseRunResult::reclaimed_bytes` é bytes de verdade em
    // `create_delete_embedded` (encolhimento de arquivo), então não reusamos
    // o campo aqui só para não inventar uma unidade errada. `reclaimed`
    // continua disponível para quem ler este código, só não persiste no
    // schema (nenhum campo hoje representa "registros recuperados pelo GC").
    (void)reclaimed;
    result.status = "completed";
    result.ok = true;
    return result;
}

namespace {

std::vector<std::byte> deterministic_blob_pattern(std::uint64_t seed, std::size_t bytes) {
    std::vector<std::byte> data;
    data.reserve(bytes);
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (std::size_t i = 0; i < bytes; ++i) {
        data.push_back(static_cast<std::byte>(byte_dist(rng)));
    }
    return data;
}

} // namespace

CaseRunResult run_blob_lifecycle_embedded(const WorkloadParams& params,
                                          std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, "blob_lifecycle");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    AttachedDatabase attached;
    if (auto setup = setup_database(attached, db_path); !setup.ok) {
        result.status = "failed";
        result.error = setup.error;
        return result;
    }

    // Tamanhos representativos -- reduzidos de 1/16/256 MiB (§4.2.1) para
    // 64 KiB/1 MiB/16 MiB nesta subfase (256 MiB por caso deixaria a
    // verificação de rotina minutos mais lenta sem exercitar nenhum código
    // adicional -- a cadeia de páginas BLBP já é exercitada de sobra a
    // partir de poucas centenas de KiB).
    const struct { const char* label; std::size_t bytes; } sizes[] = {
        {"64KiB", 64 * 1024},
        {"1MiB", 1024 * 1024},
        {"16MiB", 16 * 1024 * 1024},
    };

    // Blob "solto", sem objeto pai que o referencie -- BlobStore não exige
    // um (ADR permite blobs órfãos, usados por outras coleções por cima).
    auto blobs = attached.database->blobs();

    struct BlobLifecycle {
        std::string label;
        BlobId id{};
        std::vector<std::byte> current_content;
    };
    std::vector<BlobLifecycle> lifecycles;
    std::uint64_t errors = 0;

    // create
    RssTracker create_rss;
    const auto create_start = std::chrono::steady_clock::now();
    for (const auto& size : sizes) {
        create_rss.sample();
        const auto content = deterministic_blob_pattern(params.seed ^ 0x8106'0001ULL, size.bytes);
        auto tx = attached.database->begin();
        if (!tx) {
            ++errors;
            continue;
        }
        auto id = blobs.create(content);
        if (!id || !tx->commit()) {
            ++errors;
            continue;
        }
        lifecycles.push_back({size.label, *id, content});
    }
    const auto create_end = std::chrono::steady_clock::now();
    if (errors > 0 || lifecycles.size() != std::size(sizes)) {
        result.status = "failed";
        result.error = "blob_lifecycle: " + std::to_string(errors) + " erro(s) na criação";
        return result;
    }
    std::error_code size_error;
    const auto db_bytes_after_create = std::filesystem::file_size(db_path, size_error);
    PhaseMetrics create_phase;
    create_phase.phase = "create";
    create_phase.operations = lifecycles.size();
    create_phase.duration_ns = ns_between(create_start, create_end);
    create_phase.db_bytes = size_error ? 0 : db_bytes_after_create;
    create_phase.peak_rss_bytes = create_rss.peak();
    result.phases.push_back(create_phase);

    // read: buffer inteiro (`read`) e streaming por chunks (`read_chunks`) --
    // os dois precisam bater byte a byte com o que foi escrito.
    std::uint64_t read_mismatches = 0;
    RssTracker read_rss;
    const auto read_start = std::chrono::steady_clock::now();
    for (const auto& lc : lifecycles) {
        read_rss.sample();
        auto full = blobs.read(lc.id);
        if (!full || *full != lc.current_content) {
            ++read_mismatches;
            continue;
        }
        std::vector<std::byte> streamed;
        streamed.reserve(lc.current_content.size());
        auto streamed_ok = blobs.read_chunks(lc.id, [&](std::span<const std::byte> chunk) -> Result<void> {
            streamed.insert(streamed.end(), chunk.begin(), chunk.end());
            return {};
        });
        if (!streamed_ok || streamed != lc.current_content) {
            ++read_mismatches;
        }
    }
    const auto read_end = std::chrono::steady_clock::now();
    PhaseMetrics read_phase;
    read_phase.phase = "read";
    read_phase.operations = lifecycles.size();
    read_phase.duration_ns = ns_between(read_start, read_end);
    read_phase.errors = read_mismatches;
    read_phase.peak_rss_bytes = read_rss.peak();
    result.phases.push_back(read_phase);

    // grow: reescreve cada blob com 1,5x o conteúdo original (conteúdo novo,
    // não uma extensão do antigo -- rewrite não concatena).
    std::uint64_t grow_mismatches = 0;
    RssTracker grow_rss;
    const auto grow_start = std::chrono::steady_clock::now();
    for (auto& lc : lifecycles) {
        grow_rss.sample();
        const auto grown_size = lc.current_content.size() + lc.current_content.size() / 2;
        const auto grown = deterministic_blob_pattern(params.seed ^ 0x8106'0002ULL, grown_size);
        auto tx = attached.database->begin();
        if (!tx) {
            ++grow_mismatches;
            continue;
        }
        auto rewritten = blobs.rewrite(lc.id, grown);
        if (!rewritten || !tx->commit()) {
            ++grow_mismatches;
            continue;
        }
        lc.id = *rewritten;
        lc.current_content = grown;
        auto read_back = blobs.read(lc.id);
        if (!read_back || *read_back != lc.current_content) {
            ++grow_mismatches;
        }
    }
    const auto grow_end = std::chrono::steady_clock::now();
    std::error_code grow_size_error;
    const auto db_bytes_after_grow = std::filesystem::file_size(db_path, grow_size_error);
    PhaseMetrics grow_phase;
    grow_phase.phase = "update_grow";
    grow_phase.operations = lifecycles.size();
    grow_phase.duration_ns = ns_between(grow_start, grow_end);
    grow_phase.errors = grow_mismatches;
    grow_phase.db_bytes = grow_size_error ? 0 : db_bytes_after_grow;
    grow_phase.peak_rss_bytes = grow_rss.peak();
    result.phases.push_back(grow_phase);

    // shrink: reescreve com a metade do conteúdo pós-grow.
    std::uint64_t shrink_mismatches = 0;
    RssTracker shrink_rss;
    const auto shrink_start = std::chrono::steady_clock::now();
    for (auto& lc : lifecycles) {
        shrink_rss.sample();
        const auto shrunk_size = std::max<std::size_t>(1, lc.current_content.size() / 2);
        const auto shrunk = deterministic_blob_pattern(params.seed ^ 0x8106'0003ULL, shrunk_size);
        auto tx = attached.database->begin();
        if (!tx) {
            ++shrink_mismatches;
            continue;
        }
        auto rewritten = blobs.rewrite(lc.id, shrunk);
        if (!rewritten || !tx->commit()) {
            ++shrink_mismatches;
            continue;
        }
        lc.id = *rewritten;
        lc.current_content = shrunk;
        auto read_back = blobs.read(lc.id);
        if (!read_back || *read_back != lc.current_content) {
            ++shrink_mismatches;
        }
    }
    const auto shrink_end = std::chrono::steady_clock::now();
    PhaseMetrics shrink_phase;
    shrink_phase.phase = "update_shrink";
    shrink_phase.operations = lifecycles.size();
    shrink_phase.duration_ns = ns_between(shrink_start, shrink_end);
    shrink_phase.errors = shrink_mismatches;
    shrink_phase.peak_rss_bytes = shrink_rss.peak();
    result.phases.push_back(shrink_phase);

    // delete: remove todos e confirma que nenhum lê de volta.
    std::uint64_t still_resolving = 0;
    std::uint64_t delete_errors = 0;
    std::error_code pre_delete_size_error;
    const auto db_bytes_before_delete = std::filesystem::file_size(db_path, pre_delete_size_error);
    RssTracker delete_rss;
    const auto delete_start = std::chrono::steady_clock::now();
    for (const auto& lc : lifecycles) {
        delete_rss.sample();
        auto tx = attached.database->begin();
        if (!tx) {
            ++delete_errors;
            continue;
        }
        if (auto removed = blobs.remove(lc.id); !removed || !tx->commit()) {
            ++delete_errors;
            continue;
        }
        if (blobs.read(lc.id)) {
            ++still_resolving;
        }
    }
    const auto delete_end = std::chrono::steady_clock::now();
    std::error_code post_delete_size_error;
    const auto db_bytes_after_delete = std::filesystem::file_size(db_path, post_delete_size_error);
    // Honesto: o MVP de BlobStore não tem free list (páginas removidas ficam
    // órfãs no arquivo, comentário de blob_store.hpp) -- "espaço recuperado"
    // não é um encolhimento de arquivo aqui, é só a diferença (quase sempre
    // 0 ou negativa) entre antes e depois do delete. Não inventamos um
    // reclaimed_bytes positivo que o motor não entrega.
    const auto reclaimed = db_bytes_before_delete > db_bytes_after_delete
                              ? db_bytes_before_delete - db_bytes_after_delete
                              : 0;
    PhaseMetrics delete_phase;
    delete_phase.phase = "delete";
    delete_phase.operations = lifecycles.size();
    delete_phase.duration_ns = ns_between(delete_start, delete_end);
    delete_phase.errors = delete_errors;
    delete_phase.db_bytes = post_delete_size_error ? 0 : db_bytes_after_delete;
    delete_phase.peak_rss_bytes = delete_rss.peak();
    result.phases.push_back(delete_phase);

    result.all_deleted = still_resolving == 0 && delete_errors == 0;
    result.still_resolving = still_resolving;
    result.reclaimed_bytes = reclaimed;
    result.hash_match =
        read_mismatches == 0 && grow_mismatches == 0 && shrink_mismatches == 0 && result.all_deleted;

    if (!result.hash_match) {
        result.status = "failed";
        std::ostringstream err;
        err << "blob_lifecycle: read_mismatches=" << read_mismatches
            << " grow_mismatches=" << grow_mismatches << " shrink_mismatches=" << shrink_mismatches
            << " still_resolving=" << still_resolving << " delete_errors=" << delete_errors;
        result.error = err.str();
        return result;
    }

    result.total_duration_ns = create_phase.duration_ns + read_phase.duration_ns +
                               grow_phase.duration_ns + shrink_phase.duration_ns +
                               delete_phase.duration_ns;
    result.peak_disk_bytes = grow_phase.db_bytes;   // pico esperado é logo após o grow
    // Validação já é byte a byte em cada estágio (`read_mismatches` etc.),
    // não por um único hash de conjunto como create_only/crud_full -- deixar
    // expected_hash/actual_hash vazios é mais honesto que fabricar um par
    // trivialmente igual só para preencher o campo.
    result.status = "completed";
    result.ok = true;
    return result;
}

namespace {

// Árvore N-ária codificada como "primeiro filho / próximo irmão" (2 campos
// `OwnedRef` cobrem qualquer largura -- remover um nó em cascata já remove
// seu `first_child`, que em cascata remove seu próprio `next_sibling`, que
// remove o dele, e assim por diante: toda a subárvore + todos os irmãos).
struct HierarchyNode {
    std::int64_t id{};
    OwnedRef<HierarchyNode> first_child{};
    OwnedRef<HierarchyNode> next_sibling{};
};

BindingBuilder<HierarchyNode> hierarchy_node_binding() {
    BindingBuilder<HierarchyNode> builder{"HierarchyNode"};
    builder.field<1>("id", &HierarchyNode::id)
        .field<2>("first_child", &HierarchyNode::first_child)
        .field<3>("next_sibling", &HierarchyNode::next_sibling);
    return builder;
}

struct BuildResult {
    ObjectId id;
    std::uint64_t node_count{};
};

// Constrói de baixo para cima (filhos antes do pai -- `OwnedRef.target`
// precisa apontar pra algo que já existe) e de trás pra frente entre irmãos
// (cada nó recebe o `next_sibling` já pronto, então nenhum precisa de
// update depois de criado). `next_sibling_id` é o que ESTE nó deve apontar
// como próximo irmão -- vem do chamador, não é decidido aqui.
//
// `tx`/`created_since_commit` são compartilhados por toda a recursão (por
// referência) para respeitar `batch` (§4.5): sem isso, a árvore inteira
// (até ~1,08M nós em object_count=1M) ficaria numa única transação, com
// toda página suja seguindo no buffer de transação até um commit só no
// final. O ponto de corte -- logo depois de `database.create` retornar --
// é seguro em qualquer profundidade porque só existem referências para
// TRÁS (um nó só referencia filhos já criados, nunca o contrário), então
// nunca fecha uma transação no meio de um nó com filhos pendentes.
Result<BuildResult> build_hierarchy_subtree(Database& database, std::optional<Transaction>& tx,
                                            std::uint64_t depth, std::uint64_t width,
                                            ObjectId next_sibling_id,
                                            std::uint64_t& next_logical_id, std::uint64_t batch,
                                            std::uint64_t& created_since_commit) {
    ObjectId children_head{};
    std::uint64_t descendants = 0;
    if (depth > 0) {
        ObjectId sibling_so_far{};
        for (std::uint64_t i = 0; i < width; ++i) {
            auto built = build_hierarchy_subtree(database, tx, depth - 1, width, sibling_so_far,
                                                 next_logical_id, batch, created_since_commit);
            if (!built) {
                return std::unexpected(built.error());
            }
            sibling_so_far = built->id;
            descendants += built->node_count;
        }
        children_head = sibling_so_far;
    }

    HierarchyNode node;
    node.id = static_cast<std::int64_t>(++next_logical_id);
    node.first_child = OwnedRef<HierarchyNode>{children_head};
    node.next_sibling = OwnedRef<HierarchyNode>{next_sibling_id};
    auto created = database.create(*tx, node);
    if (!created) {
        return std::unexpected(created.error());
    }
    const auto id = created->id();

    if (++created_since_commit >= batch) {
        auto committed = tx->commit();
        if (!committed) {
            return std::unexpected(committed.error());
        }
        auto next_tx = database.begin();
        if (!next_tx) {
            return std::unexpected(next_tx.error());
        }
        tx.emplace(std::move(*next_tx));
        created_since_commit = 0;
    }
    return BuildResult{id, descendants + 1};
}

} // namespace

CaseRunResult run_cascade_delete_embedded(const WorkloadParams& params,
                                          std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, "cascade_delete");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    AttachedDatabase attached;
    if (auto setup = setup_database(attached, db_path); !setup.ok) {
        result.status = "failed";
        result.error = setup.error;
        return result;
    }
    if (auto bound = attached.database->bind(hierarchy_node_binding()); !bound) {
        result.status = "failed";
        result.error = "bind(HierarchyNode): " + bound.error().message;
        return result;
    }

    // largura^profundidade ~ object_count -- profundidade fixa em 4 (árvore
    // rasa e larga o bastante pra exercitar cascata de verdade sem recursão
    // C++ funda demais), largura derivada do object_count pedido.
    constexpr std::uint64_t kDepth = 4;
    const auto width = std::max<std::uint64_t>(
        2, static_cast<std::uint64_t>(std::llround(
               std::pow(static_cast<double>(std::max<std::uint64_t>(params.object_count, 16)),
                       1.0 / static_cast<double>(kDepth)))));

    std::uint64_t next_logical_id = 0;
    ObjectId root_id;
    std::uint64_t total_created = 0;

    // A recursão de build_hierarchy_subtree não tem ponto de tick por nó, então
    // aqui o pico é limitado por início/fim da fase.
    RssTracker create_rss;
    const auto create_start = std::chrono::steady_clock::now();
    {
        auto first_tx = attached.database->begin();
        if (!first_tx) {
            result.status = "failed";
            result.error = "begin(create_hierarchy): " + first_tx.error().message;
            return result;
        }
        std::optional<Transaction> tx{std::move(*first_tx)};
        std::uint64_t created_since_commit = 0;
        const auto batch = params.batch == 0 ? std::numeric_limits<std::uint64_t>::max() : params.batch;
        auto built = build_hierarchy_subtree(*attached.database, tx, kDepth, width, ObjectId{},
                                             next_logical_id, batch, created_since_commit);
        if (!built) {
            result.status = "failed";
            result.error = "create_hierarchy: " + built.error().message;
            return result;
        }
        if (auto committed = tx->commit(); !committed) {
            result.status = "failed";
            result.error = "commit(create_hierarchy): " + committed.error().message;
            return result;
        }
        root_id = built->id;
        total_created = built->node_count;
    }
    const auto create_end = std::chrono::steady_clock::now();

    std::uint64_t live_before_delete = 0;
    for (auto& row : attached.database->query<HierarchyNode>().stream()) {
        if (row) {
            ++live_before_delete;
        }
    }

    std::error_code size_error;
    const auto db_bytes_after_create = std::filesystem::file_size(db_path, size_error);
    PhaseMetrics create_phase;
    create_phase.phase = "create_hierarchy";
    create_phase.operations = total_created;
    create_phase.duration_ns = ns_between(create_start, create_end);
    create_phase.db_bytes = size_error ? 0 : db_bytes_after_create;
    create_phase.peak_rss_bytes = create_rss.peak();
    result.phases.push_back(create_phase);

    if (live_before_delete != total_created) {
        result.status = "failed";
        result.error = "cascade_delete: contagem após criação (" + std::to_string(live_before_delete) +
                       ") != total criado (" + std::to_string(total_created) + ")";
        return result;
    }

    // cascade_delete (raiz): uma única chamada -- o motor cai em cascata
    // por composição, nenhum passeio manual do grafo aqui (§4.2.1).
    // Remoção em cascata é UMA chamada atômica do motor: sem ponto de tick
    // interno, o pico é limitado por início/fim.
    RssTracker delete_rss;
    const auto delete_start = std::chrono::steady_clock::now();
    auto tx = attached.database->begin();
    if (!tx) {
        result.status = "failed";
        result.error = "begin(cascade_delete): " + tx.error().message;
        return result;
    }
    auto removed = attached.database->remove(*tx, root_id);
    if (!removed) {
        result.status = "failed";
        result.error = "cascade_delete(raiz): " + removed.error().message;
        return result;
    }
    if (auto committed = tx->commit(); !committed) {
        result.status = "failed";
        result.error = "commit(cascade_delete): " + committed.error().message;
        return result;
    }
    const auto delete_end = std::chrono::steady_clock::now();

    std::uint64_t live_after_delete = 0;
    for (auto& row : attached.database->query<HierarchyNode>().stream()) {
        if (row) {
            ++live_after_delete;
        }
    }

    result.all_deleted = live_after_delete == 0;
    result.still_resolving = live_after_delete;
    result.hash_match = result.all_deleted;   // "zero refs órfãs" -- não há hash lógico aqui

    if (!result.hash_match) {
        result.status = "failed";
        result.error = "cascade_delete: " + std::to_string(live_after_delete) +
                       " nó(s) órfão(s) sobrando depois da remoção em cascata da raiz (esperado: 0, "
                       "total criado=" +
                       std::to_string(total_created) + ")";
        return result;
    }

    PhaseMetrics delete_phase;
    delete_phase.phase = "cascade_delete";
    delete_phase.operations = total_created;
    delete_phase.duration_ns = ns_between(delete_start, delete_end);
    delete_phase.peak_rss_bytes = delete_rss.peak();
    result.phases.push_back(delete_phase);

    result.total_duration_ns = create_phase.duration_ns + delete_phase.duration_ns;
    result.peak_disk_bytes = create_phase.db_bytes;
    result.status = "completed";
    result.ok = true;
    return result;
}

CaseRunResult run_oversubscribed_churn_embedded(const WorkloadParams& params,
                                                std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, "oversubscribed_churn");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    // Cache deliberadamente pequeno: uma fração do working set estimado
    // (bytes/objeto ~458, medido nas outras subfases de create_delete) --
    // 10% do necessário força eviction de verdade, não um cache "quase
    // suficiente" que mascararia a diferença entre gracioso e catastrófico.
    const auto estimated_pages = std::max<std::uint64_t>(
        16, (params.object_count * 458) / modb::storage::page_size);
    const auto cache_capacity_pages =
        static_cast<std::size_t>(std::max<std::uint64_t>(8, estimated_pages / 10));

    AttachedDatabase attached;
    if (auto setup = setup_database(attached, db_path, cache_capacity_pages); !setup.ok) {
        result.status = "failed";
        result.error = setup.error;
        return result;
    }

    auto create_outcome = perform_create_phase(attached, params, db_path, wal_path, result.windows);
    if (!create_outcome.ok) {
        result.status = "failed";
        result.error = create_outcome.error;
        return result;
    }
    result.phases.push_back(create_outcome.phase);

    // Reseta as métricas do buffer pool DEPOIS do create -- o que importa
    // aqui é a razão de eviction/releitura durante o DELETE intercalado, não
    // diluída pelo preenchimento inicial do cache.
    auto& buffer_pool = attached.database->page_file().buffer_pool();
    buffer_pool.reset_metrics();

    const auto delete_ids = reorder_for_delete(create_outcome.ids, DeleteOrder::Interleaved);
    auto delete_outcome = perform_delete_phase(attached, delete_ids, params.batch, db_path, wal_path,
                                              params.on_progress, params.window_interval,
                                              result.windows);

    const auto metrics = buffer_pool.metrics();
    const auto total_accesses = metrics.hits + metrics.misses;
    delete_outcome.phase.cache_hit_rate =
        total_accesses > 0 ? static_cast<double>(metrics.hits) / static_cast<double>(total_accesses)
                          : -1.0;
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
    result.space_amplification = 0.0;   // tudo deletado -- mesma convenção de create_delete_embedded
    result.hash_match = true;   // invariantes já conferidas acima (all_deleted); sem hash de conjunto
    result.status = "completed";
    result.ok = true;
    return result;
}

CaseRunResult run_restart_recovery_embedded(const WorkloadParams& params,
                                            std::filesystem::path& out_db_path) {
    CaseRunResult result;
    const auto db_path = make_db_path(params, "restart_recovery");
    out_db_path = db_path;
    const std::filesystem::path wal_path{db_path.string() + ".wal"};

    std::vector<ObjectId> ids;
    std::unordered_map<std::uint64_t, GeneratedUser> expected;
    PhaseMetrics create_phase;

    // Escopo próprio: fecha (detach) o `Database` original ao sair, para que
    // a reabertura logo abaixo dispare o caminho de replay de WAL de
    // verdade, não reaproveite o objeto em memória.
    {
        AttachedDatabase attached;
        if (auto setup = setup_database(attached, db_path); !setup.ok) {
            result.status = "failed";
            result.error = setup.error;
            return result;
        }
        auto create_outcome = perform_create_phase(attached, params, db_path, wal_path, result.windows);
        if (!create_outcome.ok) {
            result.status = "failed";
            result.error = create_outcome.error;
            return result;
        }
        create_phase = create_outcome.phase;
        ids = create_outcome.ids;
        // Registrado já aqui, não só no fim: as ~10 saídas de erro entre
        // este ponto e o fim da função (churn, commit interrompido,
        // reabertura) não podem deixar um caso falho reportar zero fases
        // quando a criação em si já terminou com sucesso.
        result.phases.push_back(create_phase);

        const auto filler_bytes = filler_bytes_for_payload(params.payload);
        const auto churn_count = std::min<std::uint64_t>(
            ids.size(), std::max<std::uint64_t>(10, params.object_count / 10));
        if (churn_count < 1) {
            result.status = "failed";
            result.error = "restart_recovery: working set pequeno demais para churn";
            return result;
        }

        // Churn normal (commit completo) em todos menos o último -- estabelece
        // um "último commit durável" diferente do valor de criação original.
        for (std::uint64_t i = 0; i + 1 < churn_count; ++i) {
            const auto logical_id = i + 1;
            const auto new_value = generate_user_ex(params.seed, logical_id, filler_bytes,
                                                     static_cast<std::int32_t>((logical_id + 1) % 3));
            auto handle = attached.database->get<User>(ids[i]);
            if (!handle) {
                result.status = "failed";
                result.error = "churn: get falhou no id lógico " + std::to_string(logical_id);
                return result;
            }
            auto tx = attached.database->begin();
            if (!tx || !attached.database->update(*tx, *handle, to_engine_user(new_value)) ||
                !tx->commit()) {
                result.status = "failed";
                result.error = "churn: update/commit falhou no id lógico " + std::to_string(logical_id);
                return result;
            }
            expected[logical_id] = new_value;
        }

        // O último: commit PROPOSITALMENTE interrompido -- durável no WAL
        // (registro de commit gravado), mas as páginas de dados nunca
        // chegam a ser aplicadas (`CommitPhase::stop_after_commit_record`,
        // a mesma costura de teste que `tests/recovery_test.cpp` usa). É
        // exatamente a janela que `restart_recovery` precisa provar que o
        // replay de WAL cobre.
        const auto interrupted_logical_id = churn_count;
        const auto interrupted_value =
            generate_user_ex(params.seed, interrupted_logical_id, filler_bytes,
                            static_cast<std::int32_t>((interrupted_logical_id + 2) % 3));
        auto handle = attached.database->get<User>(ids[interrupted_logical_id - 1]);
        if (!handle) {
            result.status = "failed";
            result.error = "commit interrompido: get falhou";
            return result;
        }
        auto tx = attached.database->begin();
        if (!tx) {
            result.status = "failed";
            result.error = "commit interrompido: begin falhou: " + tx.error().message;
            return result;
        }
        if (auto updated = attached.database->update(*tx, *handle, to_engine_user(interrupted_value));
            !updated) {
            result.status = "failed";
            result.error = "commit interrompido: update falhou: " + updated.error().message;
            return result;
        }
        auto committed = tx->commit(CommitPhase::stop_after_commit_record);
        if (!committed) {
            result.status = "failed";
            result.error =
                "commit(stop_after_commit_record) falhou: " + committed.error().message;
            return result;
        }
        expected[interrupted_logical_id] = interrupted_value;
        // `attached` sai de escopo aqui -- detach do registro; o arquivo
        // fica exatamente como uma queda de processo real teria deixado.
    }

    // "Reinício": reabre do MESMO arquivo -- dispara o replay de WAL de
    // verdade (o mesmo `Database::open` que um processo relançado chamaria).
    const auto restart_start = std::chrono::steady_clock::now();
    AttachedDatabase reopened;
    auto opened = Database::open(db_path);
    if (!opened) {
        result.status = "failed";
        result.error = "Database::open (reinício): " + opened.error().message;
        return result;
    }
    reopened.database = std::make_shared<Database>(std::move(*opened));
    auto reopened_id = DatabaseRegistry::instance().attach(reopened.database);
    if (!reopened_id) {
        result.status = "failed";
        result.error = "DatabaseRegistry::attach (reinício): " + reopened_id.error().message;
        return result;
    }
    reopened.id = *reopened_id;
    if (auto bound = reopened.database->bind(user_binding()); !bound) {
        result.status = "failed";
        result.error = "bind(User) (reinício): " + bound.error().message;
        return result;
    }
    const auto restart_end = std::chrono::steady_clock::now();

    // Verificação (§4.2.1): hash lógico pós-recuperação == último commit
    // durável -- objetos sem churn mantêm o valor de criação, os com churn
    // (inclusive o interrompido) refletem o novo valor.
    std::uint64_t mismatches = 0;
    for (std::uint64_t i = 0; i < ids.size(); ++i) {
        const auto logical_id = i + 1;
        auto handle = reopened.database->get<User>(ids[i]);
        Result<User> value;
        if (handle) {
            value = reopened.database->materialize(*handle);
        }
        if (!handle || !value) {
            ++mismatches;
            continue;
        }
        const auto actual_line = canonical_line(from_engine_user(*value));
        std::string expected_line;
        if (auto it = expected.find(logical_id); it != expected.end()) {
            expected_line = canonical_line(it->second);
        } else {
            expected_line = canonical_line(generate_user(params.seed, logical_id, params.payload));
        }
        if (actual_line != expected_line) {
            ++mismatches;
        }
    }

    result.hash_match = mismatches == 0;
    if (!result.hash_match) {
        result.status = "failed";
        result.error = "restart_recovery: " + std::to_string(mismatches) + "/" +
                       std::to_string(ids.size()) +
                       " objeto(s) não conferem com o estado esperado pós-recuperação";
        return result;
    }

    PhaseMetrics recovery_phase;
    recovery_phase.phase = "restart_recovery";
    recovery_phase.operations = ids.size();
    recovery_phase.duration_ns = ns_between(restart_start, restart_end);
    result.phases.push_back(recovery_phase);
    result.total_duration_ns = create_phase.duration_ns + recovery_phase.duration_ns;
    result.peak_disk_bytes = create_phase.db_bytes;
    result.status = "completed";
    result.ok = true;
    return result;
}

} // namespace modb::loadtest
