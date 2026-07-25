#pragma once

// Tipos comuns entre workloads e alvos de execução (docs/PLANO_TESTES_DE_CARGA.md
// §4.3/§14). Nesta subfase só existe `target_embedded`; `target_client.hpp`
// (loopback/remoto) chega na Subfase G reaproveitando os mesmos estes tipos.

#include <cstdint>
#include <string>
#include <vector>

namespace modb::loadtest {

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
};

} // namespace modb::loadtest
