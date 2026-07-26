#include "history/retention.hpp"

#include "history/baselines.hpp"
#include "json_value.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace modb::loadtest {
namespace {

struct RawPoint {
    std::string series_key;
    std::string run_id;
    std::string status;
    std::string started_at;
    std::string raw_file;
};

} // namespace

PruneResult prune_raw_files(const PruneOptions& options) {
    PruneResult result;

    if (!std::filesystem::exists(options.history_path)) {
        result.error = "arquivo histórico não encontrado: " + options.history_path.string();
        return result;
    }
    std::ifstream file(options.history_path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();

    std::vector<RawPoint> points;
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
            result.error = options.history_path.string() + ":" + std::to_string(line_no) +
                           ": linha não é JSON válido (" + parsed.error + ")";
            return result;
        }
        RawPoint point;
        point.series_key = parsed.value.get_string("series_key");
        point.run_id = parsed.value.get_string("run_id");
        point.status = parsed.value.get_string("status");
        point.started_at = parsed.value.get_string("started_at");
        point.raw_file = parsed.value.get_string("raw_file");
        points.push_back(std::move(point));
    }

    std::vector<BaselineEntry> baselines;
    if (!options.baselines_path.empty() && std::filesystem::exists(options.baselines_path)) {
        auto loaded = load_baselines(options.baselines_path);
        if (!loaded.ok) {
            result.error = loaded.error;
            return result;
        }
        baselines = std::move(loaded.entries);
    }

    // Agrupa por série preservando a ordem cronológica dentro de cada grupo
    // -- `series.jsonl` já é append-only, mas ordena de qualquer forma para
    // não depender da ordem de escrita coincidir com `started_at`.
    std::map<std::string, std::vector<RawPoint>> by_series;
    for (auto& point : points) {
        by_series[point.series_key].push_back(point);
    }

    for (auto& [series_key, series_points] : by_series) {
        std::stable_sort(series_points.begin(), series_points.end(),
                         [](const RawPoint& a, const RawPoint& b) {
                             return a.started_at < b.started_at;
                         });

        const auto total = series_points.size();
        const auto keep_from =
            total > options.keep ? total - static_cast<std::size_t>(options.keep) : 0;

        for (std::size_t i = 0; i < total; ++i) {
            const auto& point = series_points[i];
            PruneCandidate candidate;
            candidate.series_key = series_key;
            candidate.run_id = point.run_id;
            candidate.raw_file = point.raw_file;

            if (i >= keep_from) {
                candidate.kept = true;
                candidate.reason = "entre os " + std::to_string(options.keep) +
                                   " mais recentes da série";
            } else if (point.status == "failed") {
                candidate.kept = true;
                candidate.reason = "status=failed nunca é removido (§13.8)";
            } else if (is_baseline_run(baselines, point.run_id)) {
                candidate.kept = true;
                candidate.reason = "run_id marcado como baseline (§13.9)";
            } else if (point.raw_file.empty()) {
                candidate.kept = true;
                candidate.reason = "rollup sem 'raw_file' -- nada para remover";
            } else {
                candidate.kept = false;
                candidate.reason = "mais antigo que os " + std::to_string(options.keep) +
                                   " mais recentes, não é failed nem baseline";
            }
            result.candidates.push_back(std::move(candidate));
        }
    }

    // `raw_file` é o arquivo de campanha inteiro, compartilhado por TODAS as
    // séries que tiveram algum caso naquela campanha (rollup.cpp emite um
    // rollup por caso, todos citando o mesmo raw_file) -- mas cada série
    // acima decidiu kept/not-kept olhando só a PRÓPRIA janela de recência,
    // sem saber que outra série ainda depende do mesmo arquivo. Uma campanha
    // "velha" para a série A mas ainda entre as N mais recentes da série B
    // não pode ser apagada: uni os vereditos por raw_file antes de decidir
    // (e antes de reportar em modo dry-run, para não prometer uma remoção
    // que não vai acontecer).
    std::set<std::string> raw_files_kept_by_any_series;
    for (const auto& candidate : result.candidates) {
        if (candidate.kept && !candidate.raw_file.empty()) {
            raw_files_kept_by_any_series.insert(candidate.raw_file);
        }
    }
    for (auto& candidate : result.candidates) {
        if (!candidate.kept && raw_files_kept_by_any_series.contains(candidate.raw_file)) {
            candidate.kept = true;
            candidate.reason = "raw_file compartilhado por outra série que ainda o mantém";
        }
    }

    if (options.confirm) {
        for (const auto& candidate : result.candidates) {
            if (candidate.kept) {
                continue;
            }
            const auto path = options.raw_dir / candidate.raw_file;
            std::error_code ec;
            if (std::filesystem::remove(path, ec) && !ec) {
                result.deleted.push_back(candidate.raw_file);
            }
            // Arquivo já ausente (removido antes, ou nunca existiu) não é
            // erro -- o objetivo (não ocupar espaço) já está satisfeito.
        }
    }

    result.ok = true;
    return result;
}

} // namespace modb::loadtest
