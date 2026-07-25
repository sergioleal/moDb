#include "budget.hpp"

namespace modb::loadtest {

BudgetEstimate estimate_case(const Case&) {
    // Tabela de calibração chega na Subfase H (docs/PLANO_TESTES_DE_CARGA.md
    // §10). Até lá, toda estimativa é desconhecida -- nunca um chute.
    return BudgetEstimate{};
}

BudgetCheckResult check_campaign_budget(const BudgetLimits& limits, bool any_unknown_estimate) {
    BudgetCheckResult result;
    if (any_unknown_estimate && !limits.accept_unknown_budget) {
        result.ok = false;
        result.reason =
            "orçamento desconhecido (sem tabela de calibração, §10) -- use "
            "--accept-unknown-budget para rodar mesmo assim, ou --dry-run para só listar";
    }
    return result;
}

} // namespace modb::loadtest
