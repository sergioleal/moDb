#include "history/baselines.hpp"

#include "json_value.hpp"
#include "runner/json_util.hpp"

#include <fstream>
#include <sstream>

namespace modb::loadtest {

using modb::bench::json_string;

BaselineLoadResult load_baselines(const std::filesystem::path& path) {
    BaselineLoadResult result;
    if (!std::filesystem::exists(path)) {
        result.ok = true;   // nenhuma baseline marcada ainda -- não é erro
        return result;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        result.error = "não foi possível abrir " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    auto parsed = parse_json(buffer.str());
    if (!parsed.ok || !parsed.value.is_object()) {
        result.error = path.string() + ": JSON inválido (" + parsed.error + ")";
        return result;
    }
    const auto* entries = parsed.value.find("entries");
    if (!entries || !entries->is_array()) {
        result.error = path.string() + ": campo 'entries' ausente ou não é lista";
        return result;
    }
    for (const auto& item : entries->as_array()) {
        BaselineEntry entry;
        entry.series_key = item.get_string("series_key");
        entry.run_id = item.get_string("run_id");
        entry.case_id = item.get_string("case_id");
        entry.marked_at = item.get_string("marked_at");
        entry.reason = item.get_string("reason");
        result.entries.push_back(std::move(entry));
    }
    result.ok = true;
    return result;
}

bool append_baseline(const std::filesystem::path& path, const BaselineEntry& entry) {
    auto existing = load_baselines(path);
    if (!existing.ok) {
        return false;
    }
    existing.entries.push_back(entry);

    if (const auto parent = path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    std::ostringstream oss;
    oss << "{\"schema\":\"modb.loadtest.baselines\",\"schema_version\":1,\"entries\":[";
    for (std::size_t i = 0; i < existing.entries.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        const auto& e = existing.entries[i];
        oss << "{\"series_key\":" << json_string(e.series_key)
            << ",\"run_id\":" << json_string(e.run_id) << ",\"case_id\":" << json_string(e.case_id)
            << ",\"marked_at\":" << json_string(e.marked_at)
            << ",\"reason\":" << json_string(e.reason) << "}";
    }
    oss << "]}\n";

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << oss.str();
    return static_cast<bool>(out);
}

bool is_baseline_run(const std::vector<BaselineEntry>& entries, const std::string& run_id) {
    for (const auto& e : entries) {
        if (e.run_id == run_id) {
            return true;
        }
    }
    return false;
}

} // namespace modb::loadtest
