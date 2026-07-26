#include "process_metrics.hpp"

#include <algorithm>

#if defined(_WIN32)
#define PSAPI_VERSION 2
#include <windows.h>

#include <psapi.h>
#else
#include <cstdio>
#include <cstdlib>
#include <cstring>
#endif

namespace modb::loadtest {

#if defined(_WIN32)

std::uint64_t peak_rss_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
}

std::uint64_t current_rss_bytes() {
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return 0;
    }
    return static_cast<std::uint64_t>(counters.WorkingSetSize);
}

#else

namespace {

// Lê um campo de /proc/self/status cujo valor está em kB.
std::uint64_t status_field_bytes(const char* prefix, std::size_t prefix_length) {
    std::FILE* file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return 0;
    }
    std::uint64_t kib = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, prefix, prefix_length) == 0) {
            kib = std::strtoull(line + prefix_length, nullptr, 10);
            break;
        }
    }
    std::fclose(file);
    return kib * 1024;
}

} // namespace

std::uint64_t peak_rss_bytes() { return status_field_bytes("VmHWM:", 6); }

std::uint64_t current_rss_bytes() { return status_field_bytes("VmRSS:", 6); }

#endif

RssTracker::RssTracker() {
    peak_ = current_rss_bytes();
    last_query_ = std::chrono::steady_clock::now();
}

void RssTracker::sample() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_query_ < query_interval) {
        return;
    }
    last_query_ = now;
    peak_ = std::max(peak_, current_rss_bytes());
}

std::uint64_t RssTracker::peak() {
    last_query_ = std::chrono::steady_clock::now();
    peak_ = std::max(peak_, current_rss_bytes());
    return peak_;
}

} // namespace modb::loadtest
