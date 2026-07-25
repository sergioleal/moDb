#pragma once

// Orçamento de recursos (docs/PLANO_TESTES_DE_CARGA.md §10, calibração real
// desde a Subfase H). Uma combinação (workload,payload,scale) sem entrada na
// tabela de calibração da plataforma corrente continua "?" -- `run` exige
// `--accept-unknown-budget` antes de executar qualquer caso assim (§6.3). Os
// limites (`--max-duration`/`--max-disk-gb`/`--max-rss-mb`) só se aplicam a
// casos com estimativa conhecida: excedê-los gera `skipped_budget`, nunca
// aborta uma campanha inteira por um único caso.

#include "calibration.hpp"
#include "matrix.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace modb::loadtest {

struct BudgetEstimate {
    bool known{false};
    std::uint64_t disk_bytes{};
    std::uint64_t duration_ns{};
    std::uint64_t peak_rss_bytes{};
};

// `known=false` quando `table` não tem entrada para (c.workload,c.payload,c.scale)
// -- inclui o caso de `table` vazia (nenhum arquivo de calibração encontrado).
[[nodiscard]] BudgetEstimate estimate_case(const Case& c, const CalibrationTable& table);

struct BudgetLimits {
    std::optional<std::uint64_t> max_duration_seconds;
    std::optional<std::uint64_t> max_disk_gb;
    std::optional<std::uint64_t> max_rss_mb;
    bool accept_unknown_budget{false};
};

struct BudgetCheckResult {
    bool ok{true};
    std::string reason;   // preenchido quando !ok
};

// Verificado uma vez por campanha, antes de executar qualquer caso: sem
// `accept_unknown_budget`, toda estimativa desconhecida bloqueia o `run`
// inteiro (nunca silenciosamente ignora o orçamento).
[[nodiscard]] BudgetCheckResult check_campaign_budget(const BudgetLimits& limits,
                                                      bool any_unknown_estimate);

} // namespace modb::loadtest
