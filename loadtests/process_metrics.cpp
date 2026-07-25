#include "process_metrics.hpp"

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

#else

std::uint64_t peak_rss_bytes() {
    std::FILE* file = std::fopen("/proc/self/status", "r");
    if (file == nullptr) {
        return 0;
    }
    std::uint64_t kib = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, "VmHWM:", 6) == 0) {
            kib = std::strtoull(line + 6, nullptr, 10);
            break;
        }
    }
    std::fclose(file);
    return kib * 1024;
}

#endif

} // namespace modb::loadtest
