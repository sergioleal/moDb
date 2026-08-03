#pragma once

// Tipos comuns entre workloads e alvos de execução (docs/PLANO_TESTES_DE_CARGA.md
// §4.3/§14). Nesta subfase só existe `target_embedded`; `target_client.hpp`
// (loopback/remoto) chega na Subfase G reaproveitando os mesmos estes tipos.

#include "modb/diag/stage_profile.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace modb::loadtest {

// Classes de operação de um workload de mix (PLANO_PROFILER.md §3.2 e §4.4).
// Servem a duas coisas de uma vez: o mix ALCANÇADO (contra o configurado) e a
// atribuição de estágios por classe -- que é o que torna um `stage_profile` de
// `mixed_oltp` legível, já que hoje leitura e escrita somam no mesmo balde.
enum class OperationClass : std::size_t {
    read,
    create,
    update,
    remove,
    // A degradação silenciosa do worker: com `live_ids` vazio, read/update/delete
    // não fazem nada e a operação ainda conta como executada. Sem este contador
    // não há como saber se a corrida rodou a razão que foi pedida.
    noop,
    count_
};

inline constexpr std::size_t operation_class_count =
    static_cast<std::size_t>(OperationClass::count_);

[[nodiscard]] inline std::string_view operation_class_name(OperationClass c) noexcept {
    switch (c) {
    case OperationClass::read:
        return "read";
    case OperationClass::create:
        return "create";
    case OperationClass::update:
        return "update";
    case OperationClass::remove:
        return "delete";
    case OperationClass::noop:
        return "noop";
    case OperationClass::count_:
        break;
    }
    return "unknown";
}

struct OperationClassProfile {
    std::uint64_t operations{0};
    diag::StageSnapshot stages{};
};

using OperationClassBreakdown = std::array<OperationClassProfile, operation_class_count>;

// Uma janela de progresso dentro de uma fase longa (§12 `progress_window`).
// Emitida a cada `WorkloadParams::window_interval` (padrão 10 s, §8) --
// nunca para fases curtas, que terminam antes da primeira janela fechar.
struct ProgressWindow {
    std::string phase;
    std::uint64_t window_index{};
    std::uint64_t operations_in_window{};
    std::uint64_t elapsed_ns_in_window{};
    double ops_per_second{};
    double p99_ns{};
    std::uint64_t peak_rss_bytes{};
    std::uint64_t db_bytes{};
};

using ProgressCallback = std::function<void(const ProgressWindow&)>;

// Percentis de latência por operação, em nanossegundos (§8). Os nomes de
// campo casam exatamente com o que loadtests/dashboard/index.html lê.
struct LatencyPercentilesNs {
    double p50{};
    double p95{};
    double p99{};
    double p999{};
};

// Uma fase cronometrada e validada separadamente (§8/§9). Um caso com N fases
// produz N PhaseMetrics -- nunca um número único.
struct PhaseMetrics {
    std::string phase;
    std::uint64_t operations{};
    std::uint64_t duration_ns{};
    double ops_per_second{};
    std::uint64_t bytes_per_object{};
    std::uint64_t errors{};

    // Subfase D2 (docs-process/PLANO_IMPLEMENTACAO_CARGA.md): campos que o
    // dashboard já pressupunha e o coletor não produzia.
    LatencyPercentilesNs latency_ns;
    std::uint64_t peak_rss_bytes{};
    std::uint64_t db_bytes{};
    std::uint64_t wal_bytes{};
    std::uint64_t pages_read{};
    // Estimativa (bytes escritos / page_size) -- o motor não expõe um
    // contador de páginas escritas/reutilizadas na API pública hoje. Nomeado
    // "_estimated" de propósito, para não se passar por contador real.
    std::uint64_t pages_written_estimated{};

    // Subfase L (§4.2.1 `read_hotspot`): hits/(hits+misses) do buffer pool
    // durante a fase (`database.page_file().buffer_pool().metrics()`).
    // -1.0 = não medido nesta fase (a maioria dos workloads não mede isso).
    double cache_hit_rate{-1.0};

    // Subfase N (§4.2.1 `snapshot_hold`): registros além do conjunto vivo
    // atual -- versões `previous` que uma snapshot aberta obriga o motor a
    // reter (`database.data_record_count() - vivos_atuais`). 0 quando não
    // medido (nenhuma snapshot aberta nesta fase).
    std::uint64_t retained_versions{0};

    // Etapa 1 do plano de profiling: tempo atribuído a estágios nomeados do
    // caminho quente. Todos os totais ficam em zero num build sem
    // MODB_ENABLE_STAGE_PROFILING -- e é assim que o emissor sabe que não deve
    // gravar o registro `stage_profile`, em vez de gravar uma linha de zeros
    // que se pareceria com "medi e não achei nada".
    diag::StageSnapshot stages{};

    // PLANO_PROFILER.md §3.2/§4.4: mix alcançado e estágios por classe de
    // operação. `has_operation_classes` falso = esta fase não é de mix (a
    // maioria não é) -- ausência do bloco é deliberadamente distinta de um
    // bloco de zeros, pela mesma razão que vale para `stages`.
    bool has_operation_classes{false};
    OperationClassBreakdown operation_classes{};
    // Razão configurada para a fase, para o relatório poder comparar com a
    // alcançada. 0 quando a fase não tem essa dimensão.
    std::uint64_t reads_per_write_configured{0};
};

// Razão leitura:escrita efetivamente alcançada (§4.4). Devolve -1.0 quando não
// houve escrita nenhuma -- a razão seria infinita, e inventar um número aqui
// esconderia justamente o caso em que a corrida não fez o que foi pedido.
[[nodiscard]] inline double achieved_reads_per_write(const OperationClassBreakdown& b) noexcept {
    const auto reads = b[static_cast<std::size_t>(OperationClass::read)].operations;
    const auto writes = b[static_cast<std::size_t>(OperationClass::create)].operations +
                        b[static_cast<std::size_t>(OperationClass::update)].operations +
                        b[static_cast<std::size_t>(OperationClass::remove)].operations;
    if (writes == 0) {
        return -1.0;
    }
    return static_cast<double>(reads) / static_cast<double>(writes);
}

// Soma dos tempos atribuídos, para conferir a cobertura contra a duração da
// fase (critério de aceite da Etapa 1: >= 90%).
//
// Envelopes ficam FORA da soma: eles contêm outros estágios, então incluí-los
// contaria o mesmo nanossegundo duas vezes e poderia levar a cobertura acima de
// 100%, transformando `unattributed_ns` em lixo em vez de resultado
// (PLANO_PROFILER.md §3.1). Eles são emitidos num bloco próprio.
[[nodiscard]] inline std::uint64_t attributed_ns(const diag::StageSnapshot& stages) noexcept {
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < stages.size(); ++i) {
        if (diag::stage_is_envelope(static_cast<diag::Stage>(i))) {
            continue;
        }
        total += stages[i].elapsed_ns;
    }
    return total;
}

[[nodiscard]] inline bool has_stage_data(const diag::StageSnapshot& stages) noexcept {
    for (const auto& s : stages) {
        if (s.calls != 0) {
            return true;
        }
    }
    return false;
}

// Parâmetros efetivos que um workload recebe do caso já resolvido pela
// matriz -- não sabe se está embedded ou em rede (§14: "Workload e matriz não
// sabem se estão embedded ou em rede").
struct WorkloadParams {
    std::string work_dir;
    std::uint64_t seed{};
    std::uint64_t object_count{};
    std::uint64_t batch{1000};
    std::string payload{"normal"};
    // Subfase M: só `mixed_oltp` lê isto -- sessões concorrentes de verdade
    // (§4.5), serializadas por um mutex sobre o motor single-thread (ADR-011).
    std::uint64_t concurrency{1};
    // PLANO_PROFILER.md §4.2: leituras por escrita no mix, inteiro exato (a
    // seleção é `rng() % (reads_per_write + 1)`, não um limiar em ponto
    // flutuante). 0 = só escrita, um valor legítimo. Leitura pura não se
    // expressa aqui -- não há inteiro para "infinito" -- e não precisa: já
    // existe o workload `read_hotspot`. Só `mixed_oltp` lê isto.
    std::uint64_t reads_per_write{10};

    // Subfase F: `on_progress` nulo (padrão) = não emite progress_window,
    // igual ao comportamento de antes desta subfase. `window_interval` só
    // importa quando `on_progress` está setado -- testes usam um intervalo
    // pequeno para não esperar 10s de verdade.
    ProgressCallback on_progress;
    std::chrono::nanoseconds window_interval{std::chrono::seconds(10)};
};

// Resultado de um caso completo: fases + validação (§9) + rastreabilidade.
struct CaseRunResult {
    bool ok{false};
    std::string status;   // "completed" | "failed" | "unimplemented"
    std::string error;
    std::vector<PhaseMetrics> phases;
    std::uint64_t total_duration_ns{};
    std::uint64_t peak_disk_bytes{};
    std::string expected_hash;
    std::string actual_hash;
    bool hash_match{false};

    // bytes persistidos / bytes lógicos gerados; tamanho final do arquivo /
    // bytes lógicos vivos (§8). 0.0 quando não computável (nenhum objeto).
    double write_amplification{};
    double space_amplification{};

    // Subfase D (create_delete_*, §4.2): validação de "tudo removido" --
    // distinta da validação de hash de create_only/crud_full. `true`/`0`
    // quando o workload não faz delete (ex.: create_only), nunca um
    // falso-positivo por omissão.
    bool all_deleted{true};
    std::uint64_t still_resolving{0};
    std::uint64_t reclaimed_bytes{0};

    // Subfase F (§13.3 `windows`): inclinação intra-execução da fase mais
    // longa do caso -- ausente (has_windows=false) quando nenhuma fase
    // durou o bastante para fechar uma janela.
    struct WindowsSummary {
        bool has_windows{false};
        double first_ops_per_second{};
        double last_ops_per_second{};
        double slope_ops_per_second_per_min{};
        double first_p99_ns{};
        double last_p99_ns{};
    } windows;
};

} // namespace modb::loadtest
