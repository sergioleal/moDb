#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace modb::bench {

// Escapa uma string para JSON (aspas já incluídas no retorno).
inline std::string json_string(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[(ch >> 4) & 0x0f]);
                out.push_back(hex[ch & 0x0f]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

inline std::string json_number(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    std::ostringstream oss;
    oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
    oss.precision(17);
    oss << value;
    return oss.str();
}

inline std::string json_uint(std::uint64_t value) {
    return std::to_string(value);
}

inline std::string json_int(std::int64_t value) {
    return std::to_string(value);
}

inline std::string json_bool(bool value) {
    return value ? "true" : "false";
}

// Percentil linear em amostra ordenada (p em [0,1]).
inline double percentile_sorted(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double clamped = std::clamp(p, 0.0, 1.0);
    const double index = clamped * static_cast<double>(sorted.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(index);
    const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
    const double frac = index - static_cast<double>(lo);
    return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
}

struct LatencyStats {
    double min{};
    double p50{};
    double p90{};
    double p95{};
    double p99{};
    double max{};
    double mean{};
};

inline LatencyStats summarize_latency_ns(std::vector<double> samples) {
    LatencyStats stats{};
    if (samples.empty()) {
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    double sum = 0.0;
    for (const double value : samples) {
        sum += value;
    }
    stats.min = samples.front();
    stats.max = samples.back();
    stats.mean = sum / static_cast<double>(samples.size());
    stats.p50 = percentile_sorted(samples, 0.50);
    stats.p90 = percentile_sorted(samples, 0.90);
    stats.p95 = percentile_sorted(samples, 0.95);
    stats.p99 = percentile_sorted(samples, 0.99);
    return stats;
}

} // namespace modb::bench
