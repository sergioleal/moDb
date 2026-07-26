#include "history/gate.hpp"

#include <algorithm>
#include <sstream>

namespace modb::loadtest {
namespace {

double median_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    return (n % 2 == 0) ? (values[n / 2 - 1] + values[n / 2]) / 2.0 : values[n / 2];
}

// Reconstrói, na ordem original (mais antigo primeiro), os valores
// comparáveis da MESMA série do último ponto -- a mesma condição que
// `compute_trend` usa para alimentar sua janela de mediana móvel
// (has_value && comparable && status=="completed"), só que aqui incluindo
// o próprio candidato (deriva compara "as últimas 5 incluindo agora", não
// "candidato vs. histórico anterior" como o gate pontual).
std::vector<double> series_values(const std::vector<TrendPoint>& points,
                                  const std::string& series_key) {
    std::vector<double> values;
    for (const auto& p : points) {
        if (p.series_key == series_key && p.has_value && p.comparable && p.status == "completed") {
            values.push_back(p.value);
        }
    }
    return values;
}

} // namespace

GateResult compute_gate(const std::filesystem::path& history_path, const std::string& case_id,
                        const std::string& metric_id, const std::string& phase) {
    GateResult result;

    auto metric = find_metric(metric_id);
    if (!metric) {
        result.error = "métrica desconhecida: " + metric_id;
        return result;
    }
    auto trend = compute_trend(history_path, case_id, metric_id, phase);
    if (!trend.ok) {
        result.error = trend.error;
        return result;
    }
    result.ok = true;

    if (trend.points.empty()) {
        return result;   // insufficient/insufficient, passed=true (nada para julgar)
    }

    const auto& last = trend.points.back();

    // Gate por execução: o veredito do último ponto já É essa comparação
    // (candidato × mediana das últimas 5 ANTERIORES) -- `compute_trend` já
    // fez a conta. `status != "completed"` é falha imediata de correção
    // (§9/§13.7), nunca um "insufficient" por falta de comparação.
    if (last.status != "completed") {
        result.point_verdict = "fail";
        result.passed = false;
    } else {
        result.point_verdict = last.verdict;
        result.point_vs_median = last.vs_median;
        if (last.verdict == "fail") {
            result.passed = false;
        }
    }

    // Deriva lenta: só faz sentido comparar dentro da mesma série do
    // candidato -- um `series_break` no meio do caminho invalida a
    // comparação (§13.4), então não olhamos além dela.
    const auto values = series_values(trend.points, last.series_key);
    constexpr std::size_t kLookback = 20;
    constexpr std::size_t kWindow = 5;
    constexpr std::size_t kMinWindow = 3;
    if (values.size() >= kLookback + kMinWindow) {
        const auto reference_end = values.size() - kLookback;
        const auto reference_start = reference_end > kWindow ? reference_end - kWindow : 0;
        std::vector<double> reference(values.begin() + static_cast<long>(reference_start),
                                      values.begin() + static_cast<long>(reference_end));
        if (reference.size() >= kMinWindow) {
            const auto current_start = values.size() > kWindow ? values.size() - kWindow : 0;
            std::vector<double> current(values.begin() + static_cast<long>(current_start),
                                        values.end());
            const auto reference_median = median_of(reference);
            const auto current_median = median_of(current);
            const auto rel = (current_median - reference_median) / reference_median;
            const auto worse = metric->better_up ? -rel : rel;
            result.drift_ratio = rel;
            result.drift_verdict = worse >= 0.15 ? "fail" : "ok";
            if (result.drift_verdict == "fail") {
                result.passed = false;
            }
        }
    }

    return result;
}

std::string render_gate(const GateResult& result, const std::string& case_id,
                        const std::string& metric_id) {
    std::ostringstream oss;
    oss << "gate " << case_id << " " << metric_id << '\n';
    oss << "  pontual: " << result.point_verdict;
    if (result.point_verdict != "insufficient") {
        oss << " (" << (result.point_vs_median >= 0 ? "+" : "") << (result.point_vs_median * 100.0)
            << "% vs mediana das 5 anteriores)";
    }
    oss << '\n';
    oss << "  deriva:  " << result.drift_verdict;
    if (result.drift_verdict != "insufficient") {
        oss << " (" << (result.drift_ratio >= 0 ? "+" : "") << (result.drift_ratio * 100.0)
            << "% vs mediana de 20 execuções atrás)";
    }
    oss << '\n';
    oss << "  resultado: " << (result.passed ? "passou" : "REPROVADO") << '\n';
    return oss.str();
}

} // namespace modb::loadtest
