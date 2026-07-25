#pragma once

// Orçamento de recursos (docs/PLANO_TESTES_DE_CARGA.md §10). Sem tabela de
// calibração ainda (Subfase H): toda estimativa é desconhecida ("?"), e
// `run` exige `--accept-unknown-budget` antes de executar qualquer coisa
// (§6.3) -- os limites em si (`--max-duration`/`--max-disk-gb`/`--max-rss-mb`)
// só passam a ser aplicados quando houver estimativa real para compará-los.

#include "matrix.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace modb::loadtest {

struct BudgetEstimate {
    bool known{false};
    std::uint64_t disk_bytes{};
    std::uint64_t duration_ns{};
};

// Sempre `known=false` nesta subfase -- não há tabela de calibração (§10).
[[nodiscard]] BudgetEstimate estimate_case(const Case& c);

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
