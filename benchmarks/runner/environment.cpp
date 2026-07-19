#include "runner/environment.hpp"

#include "runner/json_util.hpp"
#include "runner/sha256.hpp"

#include "modb/version.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace modb::bench {
namespace {

std::string trim(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

std::string run_git(const char* args) {
#if defined(_WIN32)
    std::string command = "git ";
    command += args;
    command += " 2>NUL";
    FILE* pipe = _popen(command.c_str(), "r");
#else
    std::string command = "git ";
    command += args;
    command += " 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
#endif
    if (pipe == nullptr) {
        return {};
    }
    std::array<char, 256> buffer{};
    std::string out;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        out += buffer.data();
    }
#if defined(_WIN32)
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return trim(std::move(out));
}

std::string host_name_raw() {
#if defined(_WIN32)
    char buffer[256]{};
    DWORD size = static_cast<DWORD>(sizeof(buffer));
    if (GetComputerNameA(buffer, &size) != 0) {
        return std::string{buffer, size};
    }
    return "unknown";
#else
    char buffer[256]{};
    if (gethostname(buffer, sizeof(buffer)) == 0) {
        buffer[sizeof(buffer) - 1] = '\0';
        return buffer;
    }
    return "unknown";
#endif
}

} // namespace

EnvironmentInfo collect_environment(std::string_view argv_joined) {
    EnvironmentInfo info;
    info.git_commit = run_git("rev-parse HEAD");
    if (info.git_commit.size() >= 12) {
        info.git_commit_short = info.git_commit.substr(0, 12);
    } else if (!info.git_commit.empty()) {
        info.git_commit_short = info.git_commit;
    } else {
        info.git_commit = "unknown";
        info.git_commit_short = "unknown";
    }
    info.git_branch = run_git("rev-parse --abbrev-ref HEAD");
    if (info.git_branch.empty()) {
        info.git_branch = "unknown";
    }
    const auto dirty = run_git("status --porcelain");
    info.git_dirty = !dirty.empty();

#if defined(__clang__)
    info.compiler_id = "Clang";
    info.compiler_version = std::to_string(__clang_major__) + "." +
                            std::to_string(__clang_minor__) + "." +
                            std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
    info.compiler_id = "GNU";
    info.compiler_version = std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
                            std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    info.compiler_id = "MSVC";
    info.compiler_version = std::to_string(_MSC_VER);
#else
    info.compiler_id = "unknown";
    info.compiler_version = "unknown";
#endif

#if defined(__cplusplus)
    info.cxx_standard = std::to_string(__cplusplus);
#else
    info.cxx_standard = "unknown";
#endif

#if defined(NDEBUG)
    info.build_type = "Release";
#else
    info.build_type = "Debug";
#endif

#if defined(_WIN32)
    info.os_name = "Windows";
    info.os_version = std::to_string(GetVersion());
#elif defined(__linux__)
    info.os_name = "Linux";
    utsname uts{};
    if (uname(&uts) == 0) {
        info.os_version = uts.release;
    } else {
        info.os_version = "unknown";
    }
#elif defined(__APPLE__)
    info.os_name = "Darwin";
    info.os_version = "unknown";
#else
    info.os_name = "unknown";
    info.os_version = "unknown";
#endif

#if defined(_M_X64) || defined(__x86_64__)
    info.arch = "x86_64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    info.arch = "arm64";
#else
    info.arch = "unknown";
#endif

    const auto host = host_name_raw();
    info.hostname_token = sha256_hex(sha256_text(host)).substr(0, 8);

    info.page_size = std::to_string(MODB_PAGE_SIZE);
    info.project_version = std::string{modb::project_version()};
    info.argv_joined = std::string{argv_joined};
    return info;
}

std::string utc_timestamp_millis() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto secs = clock::to_time_t(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch())
                            .count() %
                        1000;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &secs);
#else
    gmtime_r(&secs, &tm);
#endif
    char buffer[32]{};
    std::snprintf(buffer, sizeof(buffer), "%04d%02d%02dT%02d%02d%02d.%03lldZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(millis < 0 ? -millis : millis));
    return buffer;
}

std::string make_run_id(std::string_view utc_stamp) {
    std::ostringstream oss;
    oss << "run-" << utc_stamp << '-' << sha256_hex(sha256_text(utc_stamp)).substr(0, 8);
    return oss.str();
}

} // namespace modb::bench
