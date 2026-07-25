#pragma once

// Consulta da série histórica (docs/PLANO_TESTES_DE_CARGA.md §13.6/§13.7).
// Mesmo registro de métricas do dashboard (loadtests/dashboard/index.html) --
// os dois precisam concordar, senão um dos dois está errado (§16).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace modb::loadtest {

struct MetricDefinition {
    std::string label;
    std::string unit;
    bool is_case_scope{false};   // false = por fase (precisa de --phase); true = totals do caso
    bool better_up{false};       // true = maior é melhor (ex.: ops_per_second)
    double alert_ratio{0.05};
    double fail_ratio{0.10};
};

// Nomes exatamente iguais ao METRICS de loadtests/dashboard/index.html.
[[nodiscard]] std::optional<MetricDefinition> find_metric(std::string_view metric_id);
[[nodiscard]] std::vector<std::string> known_metric_ids();

struct TrendPoint {
    std::string started_at;
    std::string commit_short;
    std::string series_key;
    std::string status;
    bool comparable{true};
    bool has_value{false};
    double value{};
    bool series_break{false};   // series_key diferente do ponto anterior

    // Preenchido só quando há >= 3 pontos comparáveis na janela (até 5
    // anteriores, mesma série) -- caso contrário "insufficient" (§13.6).
    std::string verdict;   // "ok" | "alert" | "fail" | "insufficient"
    double vs_median{};    // (value - mediana) / mediana; só válido se verdict != "insufficient"
};

struct TrendResult {
    bool ok{false};
    std::string error;
    std::vector<TrendPoint> points;
};

// Lê `history_path`, filtra por `case_id`, extrai `metric_id` (na fase
// `phase` quando a métrica é por fase) e calcula mediana móvel + veredito
// por ponto. Não precisa de `phase` para métricas de escopo de caso.
[[nodiscard]] TrendResult compute_trend(const std::filesystem::path& history_path,
                                        const std::string& case_id, const std::string& metric_id,
                                        const std::string& phase);

// Formata `result` como texto pronto para `modb_load trend` (uma linha por
// ponto: data, commit, valor, delta vs mediana, veredito).
[[nodiscard]] std::string render_trend(const TrendResult& result, const MetricDefinition& metric);

} // namespace modb::loadtest
