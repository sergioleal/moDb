#pragma once

// Tabela de calibração medida (docs/PLANO_TESTES_DE_CARGA.md §10, Subfase H).
// Um arquivo por plataforma+arquitetura (`loadtests/calibration/<platform>-
// <arch>.json`), preenchido por medição real -- nunca por chute. Ausência do
// arquivo, ou do (workload,payload,scale) pedido dentro dele, é "desconhecido"
// (`known=false` em `BudgetEstimate`), não um erro: nem toda combinação
// precisa estar calibrada para o `run` funcionar (com `--accept-unknown-budget`).

#include "matrix.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace modb::loadtest {

struct CalibrationPoint {
    std::string source;   // "measured" | "extrapolated_linear_from_100k" (informativo)
    std::uint64_t duration_ns{};
    std::uint64_t disk_peak_bytes{};
    std::uint64_t peak_rss_bytes{};
};

struct CalibrationEntry {
    std::string workload;
    std::string payload;
    std::vector<std::pair<std::string, CalibrationPoint>> scales;   // scale_id -> ponto
};

struct CalibrationTable {
    std::vector<CalibrationEntry> entries;

    // nullptr se a combinação (workload,payload,scale) não estiver calibrada.
    [[nodiscard]] const CalibrationPoint* find(const std::string& workload, const std::string& payload,
                                               const std::string& scale) const;
};

struct CalibrationLoadResult {
    bool ok{false};
    CalibrationTable table;   // vazia quando o arquivo não existe (não é erro)
    std::string error;        // preenchido só quando o arquivo existe mas é inválido
};

// "loadtests/calibration/windows-x86_64.json" ou "linux-x86_64.json",
// resolvido em tempo de compilação pela plataforma do binário -- é o binário
// rodando, não o SO do host, que determina qual arquivo é relevante (ex.:
// rodar sob WSL usa o binário Linux, então o arquivo Linux).
[[nodiscard]] std::filesystem::path default_calibration_path();

// Ausência do arquivo em `path` devolve `ok=true` com tabela vazia -- ainda
// não há calibração para essa plataforma, não é uma falha de `run`.
[[nodiscard]] CalibrationLoadResult load_calibration(const std::filesystem::path& path);

} // namespace modb::loadtest
