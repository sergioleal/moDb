#include "history/trend.hpp"

#include "json_value.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace modb::loadtest {
namespace {

// Espelha loadtests/dashboard/index.html `const METRICS = {...}` -- os dois
// precisam concordar (§16: "mostrar a mesma leitura que modb_load trend/gate
// para o mesmo caso e métrica -- se divergirem, um dos dois está errado").
const std::vector<std::pair<std::string, MetricDefinition>>& metric_table() {
    static const std::vector<std::pair<std::string, MetricDefinition>> table = {
        {"ops_per_second", {"Objetos por segundo", "obj/s", false, true, 0.05, 0.10}},
        {"p50", {"Latência p50", "ns", false, false, 0.05, 0.10}},
        {"p99", {"Latência p99", "ns", false, false, 0.10, 0.15}},
        {"p999", {"Latência p99.9", "ns", false, false, 0.10, 0.15}},
        {"phase_duration", {"Duração da fase", "ns", false, false, 0.05, 0.10}},
        {"bytes_per_object", {"Bytes por objeto", "B", false, false, 0.05, 0.10}},
        {"wal_bytes", {"WAL ao fim da fase", "bytes", false, false, 0.05, 0.10}},
        {"peak_rss", {"RSS de pico da fase", "bytes", false, false, 0.05, 0.10}},
        {"total_duration", {"Duração total do caso", "ns", true, false, 0.05, 0.10}},
        {"peak_disk", {"Disco de pico", "bytes", true, false, 0.05, 0.10}},
        {"write_amplification", {"Write amplification", "x", true, false, 0.05, 0.10}},
    };
    return table;
}

std::optional<double> phase_field(const JsonValue& rollup, const std::string& phase,
                                  std::string_view field) {
    const auto* phases = rollup.find("phases");
    if (!phases || !phases->is_array()) {
        return std::nullopt;
    }
    for (const auto& p : phases->as_array()) {
        if (p.get_string("phase") == phase) {
            return p.get_number(field);
        }
    }
    return std::nullopt;
}

std::optional<double> latency_field(const JsonValue& rollup, const std::string& phase,
                                    std::string_view percentile) {
    const auto* phases = rollup.find("phases");
    if (!phases || !phases->is_array()) {
        return std::nullopt;
    }
    for (const auto& p : phases->as_array()) {
        if (p.get_string("phase") != phase) {
            continue;
        }
        const auto* latency = p.find("latency_ns");
        if (!latency) {
            return std::nullopt;
        }
        return latency->get_number(percentile);
    }
    return std::nullopt;
}

std::optional<double> extract_value(const JsonValue& rollup, const std::string& metric_id,
                                    const std::string& phase) {
    if (metric_id == "ops_per_second") {
        return phase_field(rollup, phase, "ops_per_second");
    }
    if (metric_id == "p50" || metric_id == "p99" || metric_id == "p999") {
        return latency_field(rollup, phase, metric_id);
    }
    if (metric_id == "phase_duration") {
        return phase_field(rollup, phase, "duration_ns");
    }
    if (metric_id == "bytes_per_object") {
        return phase_field(rollup, phase, "bytes_per_object");
    }
    if (metric_id == "wal_bytes") {
        return phase_field(rollup, phase, "wal_bytes");
    }
    if (metric_id == "peak_rss") {
        return phase_field(rollup, phase, "peak_rss_bytes");
    }
    const auto* totals = rollup.find("totals");
    if (!totals) {
        return std::nullopt;
    }
    if (metric_id == "total_duration") {
        return totals->get_number("duration_ns");
    }
    if (metric_id == "peak_disk") {
        return totals->get_number("peak_disk_bytes");
    }
    if (metric_id == "write_amplification") {
        return totals->get_number("write_amplification");
    }
    return std::nullopt;
}

double median_of(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto n = values.size();
    return (n % 2 == 0) ? (values[n / 2 - 1] + values[n / 2]) / 2.0 : values[n / 2];
}

} // namespace

std::optional<MetricDefinition> find_metric(std::string_view metric_id) {
    for (const auto& [id, def] : metric_table()) {
        if (id == metric_id) {
            return def;
        }
    }
    return std::nullopt;
}

std::vector<std::string> known_metric_ids() {
    std::vector<std::string> ids;
    for (const auto& [id, def] : metric_table()) {
        ids.push_back(id);
    }
    return ids;
}

TrendResult compute_trend(const std::filesystem::path& history_path, const std::string& case_id,
                          const std::string& metric_id, const std::string& phase) {
    TrendResult result;

    auto metric = find_metric(metric_id);
    if (!metric) {
        result.error = "métrica desconhecida: " + metric_id;
        return result;
    }
    if (!metric->is_case_scope && phase.empty()) {
        result.error = "métrica '" + metric_id + "' é por fase -- informe --phase";
        return result;
    }

    if (!std::filesystem::exists(history_path)) {
        result.error = "arquivo histórico não encontrado: " + history_path.string();
        return result;
    }
    std::ifstream file(history_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();

    struct Row {
        std::string started_at, commit_short, series_key, status;
        bool comparable{true};
        std::optional<double> value;
    };
    std::vector<Row> rows;

    std::istringstream lines(buffer.str());
    std::string raw_line;
    std::size_t line_no = 0;
    while (std::getline(lines, raw_line)) {
        ++line_no;
        if (raw_line.empty()) {
            continue;
        }
        auto parsed = parse_json(raw_line);
        if (!parsed.ok || !parsed.value.is_object()) {
            result.error = history_path.string() + ":" + std::to_string(line_no) +
                           ": linha não é JSON válido (" + parsed.error + ")";
            return result;
        }
        if (parsed.value.get_string("case_id") != case_id) {
            continue;
        }
        Row row;
        row.started_at = parsed.value.get_string("started_at");
        row.commit_short = parsed.value.get_string("commit_short");
        row.series_key = parsed.value.get_string("series_key");
        row.status = parsed.value.get_string("status");
        row.comparable = parsed.value.get_bool("comparable", true);
        row.value = extract_value(parsed.value, metric_id, phase);
        rows.push_back(std::move(row));
    }

    std::stable_sort(rows.begin(), rows.end(),
                     [](const Row& a, const Row& b) { return a.started_at < b.started_at; });

    std::vector<double> window;   // valores comparáveis da série atual, na ordem
    std::string current_series_key;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        TrendPoint point;
        point.started_at = row.started_at;
        point.commit_short = row.commit_short;
        point.series_key = row.series_key;
        point.status = row.status;
        point.comparable = row.comparable;
        point.has_value = row.value.has_value();
        point.value = row.value.value_or(0.0);
        point.verdict = "insufficient";

        if (row.series_key != current_series_key) {
            point.series_break = !current_series_key.empty();
            current_series_key = row.series_key;
            window.clear();
        }

        if (point.has_value && point.comparable && point.status == "completed") {
            std::vector<double> trailing(window.end() - static_cast<long>(std::min<std::size_t>(5, window.size())),
                                        window.end());
            // ref == 0 é legítimo (ex.: wal_bytes/peak_disk em escalas
            // pequenas) e tornaria a razão relativa inf/nan -- sem base de
            // comparação, o veredito fica "insufficient" (valor padrão) em
            // vez de um falso "fail"/"ok".
            if (const auto ref = trailing.size() >= 3 ? median_of(trailing) : 0.0;
                trailing.size() >= 3 && ref != 0.0) {
                const auto rel = (point.value - ref) / ref;
                const auto worse = metric->better_up ? -rel : rel;
                point.vs_median = rel;
                point.verdict = worse >= metric->fail_ratio  ? "fail"
                               : worse >= metric->alert_ratio ? "alert"
                                                              : "ok";
            }
            window.push_back(point.value);
        }

        result.points.push_back(std::move(point));
    }

    result.ok = true;
    return result;
}

std::string render_trend(const TrendResult& result, const MetricDefinition& metric) {
    std::ostringstream oss;
    for (const auto& p : result.points) {
        if (p.series_break) {
            oss << "-- nova série --\n";
        }
        oss << p.started_at << "  " << p.commit_short << "  ";
        if (!p.has_value) {
            oss << "(sem valor)";
        } else {
            oss << p.value << " " << metric.unit;
            if (!p.comparable) {
                oss << "  [comparable=false]";
            }
            if (p.verdict == "insufficient") {
                oss << "  histórico insuficiente (< 3 pontos)";
            } else {
                oss << "  vs mediana: " << (p.vs_median >= 0 ? "+" : "") << (p.vs_median * 100.0)
                    << "%  " << p.verdict;
            }
        }
        if (p.status != "completed") {
            oss << "  status=" << p.status;
        }
        oss << '\n';
    }
    return oss.str();
}

} // namespace modb::loadtest
