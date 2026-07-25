#include "budget.hpp"

namespace modb::loadtest {

BudgetEstimate estimate_case(const Case& c, const CalibrationTable& table) {
    BudgetEstimate result;
    const auto* point = table.find(c.workload, c.payload, c.scale);
    if (!point) {
        return result;   // known=false -- sem calibração para esta combinação
    }
    result.known = true;
    result.disk_bytes = point->disk_peak_bytes;
    result.duration_ns = point->duration_ns;
    result.peak_rss_bytes = point->peak_rss_bytes;
    return result;
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
