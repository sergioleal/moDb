#include "calibration.hpp"

#include <string>

#include "json_value.hpp"

#include <fstream>
#include <sstream>

namespace modb::loadtest {

const CalibrationPoint* CalibrationTable::find(const std::string& workload, const std::string& payload,
                                               const std::string& scale) const {
    for (const auto& entry : entries) {
        if (entry.workload != workload || entry.payload != payload) {
            continue;
        }
        for (const auto& [scale_id, point] : entry.scales) {
            if (scale_id == scale) {
                return &point;
            }
        }
    }
    return nullptr;
}

std::filesystem::path default_calibration_path() {
#if defined(_WIN32)
    const std::string platform = "windows-x86_64";
#elif defined(__linux__)
    const std::string platform = "linux-x86_64";
#else
    const std::string platform = "unknown";
#endif
    const std::filesystem::path dir{"loadtests/calibration"};
    // Um arquivo por tipo de build (T22): a calibração em Debug superestimava a
    // duração de um binário otimizado em ~2,5x e fazia `--max-duration` pular
    // casos que caberiam. O arquivo sem sufixo é o legado, medido em Debug.
#if defined(MODB_BUILD_TYPE_NAME)
    const std::string build{MODB_BUILD_TYPE_NAME};
    if (!build.empty()) {
        const auto specific = dir / (platform + "-" + build + ".json");
        std::error_code error;
        if (std::filesystem::exists(specific, error)) {
            return specific;
        }
    }
#endif
    return dir / (platform + ".json");
}

CalibrationLoadResult load_calibration(const std::filesystem::path& path) {
    CalibrationLoadResult result;
    if (!std::filesystem::exists(path)) {
        result.ok = true;   // sem calibração ainda para esta plataforma -- não é erro
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
        result.error = path.string() + ": campo 'entries' ausente ou não é uma lista";
        return result;
    }

    for (const auto& entry_json : entries->as_array()) {
        CalibrationEntry entry;
        entry.workload = entry_json.get_string("workload");
        entry.payload = entry_json.get_string("payload");
        if (entry.workload.empty()) {
            result.error = path.string() + ": entrada de calibração sem 'workload'";
            return result;
        }

        const auto* scales = entry_json.find("scales");
        if (scales && scales->is_object()) {
            for (const auto& [scale_id, point_json] : scales->as_object()) {
                CalibrationPoint point;
                point.source = point_json.get_string("source");
                point.duration_ns = static_cast<std::uint64_t>(point_json.get_number("duration_ns"));
                point.disk_peak_bytes =
                    static_cast<std::uint64_t>(point_json.get_number("disk_peak_bytes"));
                point.peak_rss_bytes =
                    static_cast<std::uint64_t>(point_json.get_number("peak_rss_bytes"));
                entry.scales.emplace_back(scale_id, point);
            }
        }
        result.table.entries.push_back(std::move(entry));
    }

    result.ok = true;
    return result;
}

} // namespace modb::loadtest
