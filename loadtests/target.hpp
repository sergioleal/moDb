#pragma once

// Tipos comuns entre workloads e alvos de execução (docs/PLANO_TESTES_DE_CARGA.md
// §4.3/§14). Nesta subfase só existe `target_embedded`; `target_client.hpp`
// (loopback/remoto) chega na Subfase G reaproveitando os mesmos estes tipos.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace modb::loadtest {

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
};

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
