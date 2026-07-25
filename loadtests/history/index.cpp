#include "history/index.hpp"

#include "history/rollup.hpp"
#include "json_value.hpp"

#include <fstream>
#include <set>
#include <sstream>

namespace modb::loadtest {
namespace {

std::string dedup_key(const JsonValue& v) {
    return v.get_string("run_id") + "|" + v.get_string("case_id") + "|" +
          std::to_string(static_cast<std::uint64_t>(v.get_number("repeat_index")));
}

// Um rollup sem procedência é ruído que envenena a série para sempre (§13.3)
// -- recusado aqui, antes de tocar o arquivo append-only.
std::string missing_provenance_field(const JsonValue& v) {
    if (v.get_string("commit").empty()) {
        return "commit";
    }
    if (v.get_string("series_key").empty()) {
        return "series_key";
    }
    if (v.get_string("environment").empty()) {
        return "environment";
    }
    const auto* env = v.find("env");
    if (!env || env->get_string("host_class").empty()) {
        return "host_class";
    }
    if (env->get_string("build_type").empty()) {
        return "build_type";
    }
    if (v.get_string("seed").empty()) {
        return "seed";
    }
    if (v.get_string("status").empty()) {
        return "status";
    }
    return "";
}

} // namespace

IndexResult index_campaign(const std::filesystem::path& campaign_path,
                           const std::filesystem::path& history_path,
                           const std::filesystem::path& environments_file) {
    IndexResult result;

    auto extracted = extract_rollups(campaign_path, environments_file);
    if (!extracted.ok) {
        result.error = extracted.error;
        return result;
    }

    std::set<std::string> existing_keys;
    if (std::filesystem::exists(history_path)) {
        std::ifstream existing(history_path, std::ios::binary);
        std::string line;
        std::size_t line_no = 0;
        while (std::getline(existing, line)) {
            ++line_no;
            if (line.empty()) {
                continue;
            }
            auto parsed = parse_json(line);
            if (!parsed.ok || !parsed.value.is_object()) {
                result.error = history_path.string() + ":" + std::to_string(line_no) +
                               ": linha existente não é JSON válido -- corrija manualmente antes "
                               "de indexar (append-only não sobrescreve, mas também não ignora "
                               "corrupção)";
                return result;
            }
            existing_keys.insert(dedup_key(parsed.value));
        }
    }

    std::vector<std::string> to_append;
    for (const auto& rollup_line : extracted.rollup_lines) {
        auto parsed = parse_json(rollup_line);
        if (!parsed.ok || !parsed.value.is_object()) {
            result.error = "rollup gerado internamente não é JSON válido -- bug em extract_rollups: " +
                           parsed.error;
            return result;
        }
        const auto missing = missing_provenance_field(parsed.value);
        if (!missing.empty()) {
            ++result.rejected;
            result.rejection_reasons.push_back(parsed.value.get_string("case_id") +
                                              ": rollup sem '" + missing + "'");
            continue;
        }
        const auto key = dedup_key(parsed.value);
        if (existing_keys.contains(key)) {
            ++result.skipped_duplicate;
            continue;
        }
        existing_keys.insert(key);   // duas linhas da MESMA extração não duplicam entre si
        to_append.push_back(rollup_line);
    }

    if (!to_append.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(history_path.parent_path(), ec);
        std::ofstream out(history_path, std::ios::binary | std::ios::app);
        if (!out) {
            result.error = "não foi possível abrir " + history_path.string() + " para append";
            return result;
        }
        for (const auto& line : to_append) {
            out << line << '\n';
        }
        result.appended = to_append.size();
    }

    result.ok = true;
    return result;
}

} // namespace modb::loadtest
