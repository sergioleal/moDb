#include "history/excluded_runs.hpp"

#include "json_value.hpp"

#include <fstream>
#include <sstream>

namespace modb::loadtest {

ExcludedRuns load_excluded_runs(const std::filesystem::path& history_path) {
    ExcludedRuns result;
    const auto path = history_path.parent_path() / "excluded_runs.json";
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return result;
    }
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    auto parsed = parse_json(buffer.str());
    if (!parsed.ok || !parsed.value.is_object()) {
        result.error = path.string() + ": não é JSON válido (" + parsed.error + ")";
        return result;
    }
    const auto* runs = parsed.value.find("runs");
    if (runs == nullptr || !runs->is_array()) {
        result.error = path.string() + ": falta a lista 'runs'";
        return result;
    }
    for (const auto& run : runs->as_array()) {
        if (!run.is_object()) {
            continue;
        }
        const auto run_id = run.get_string("run_id");
        if (run_id.empty()) {
            continue;
        }
        const auto case_id = run.get_string("case_id");
        result.reasons[case_id.empty() ? run_id : run_id + "|" + case_id] = run.get_string("reason");
    }
    return result;
}

} // namespace modb::loadtest
