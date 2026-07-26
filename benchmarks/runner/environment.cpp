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

#if defined(_WIN32)

// Valor REG_SZ de HKLM, string vazia se ausente.
std::string registry_string(const char* subkey, const char* value_name) {
    HKEY key{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return {};
    }
    char buffer[256]{};
    DWORD size = static_cast<DWORD>(sizeof(buffer));
    DWORD type = 0;
    std::string out;
    if (RegQueryValueExA(key, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(buffer),
                         &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        out = trim(std::string{buffer});
    }
    RegCloseKey(key);
    return out;
}

// Valor REG_DWORD de HKLM; `found` distingue "ausente" de "zero".
std::uint32_t registry_dword(const char* subkey, const char* value_name, bool& found) {
    found = false;
    HKEY key{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }
    DWORD data = 0;
    DWORD size = sizeof(data);
    DWORD type = 0;
    if (RegQueryValueExA(key, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(&data), &size) ==
            ERROR_SUCCESS &&
        type == REG_DWORD) {
        found = true;
    }
    RegCloseKey(key);
    return data;
}

// Versão do Windows legível: "10.0.26200.1234 (24H2)".
//
// GetVersion() devolve um DWORD empacotado -- imprimi-lo em decimal produzia
// coisas como "1717043210", que não é versão de nada. Pior: a API é deprecada e
// mente sobre 8.1+ sem um manifesto de compatibilidade. A chave CurrentVersion
// do registro é o que o próprio sistema publica e não depende de manifesto.
std::string windows_version_string() {
    static constexpr const char* kKey = R"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)";

    bool have_major = false, have_minor = false, have_ubr = false;
    const auto major = registry_dword(kKey, "CurrentMajorVersionNumber", have_major);
    const auto minor = registry_dword(kKey, "CurrentMinorVersionNumber", have_minor);
    const auto ubr = registry_dword(kKey, "UBR", have_ubr);
    const auto build = registry_string(kKey, "CurrentBuildNumber");
    const auto display = registry_string(kKey, "DisplayVersion");

    std::string out;
    if (have_major) {
        out = std::to_string(major) + "." + std::to_string(have_minor ? minor : 0);
    }
    if (!build.empty()) {
        out += out.empty() ? build : "." + build;
    }
    if (have_ubr && !out.empty()) {
        out += "." + std::to_string(ubr);
    }
    if (!display.empty()) {
        out += out.empty() ? display : " (" + display + ")";
    }
    return out.empty() ? "unknown" : out;
}

#endif

// Modelo de CPU, núcleos físicos/lógicos e RAM. Cada campo fica no seu valor
// vazio/zero quando a consulta falha -- nenhum destes itens vale falhar uma
// campanha, e um zero explícito é lido como "não coletado" pelo rollup, ao
// contrário de um número inventado (docs/PLANO_BENCHMARKS.md §4.2).
void collect_hardware(EnvironmentInfo& info) {
#if defined(_WIN32)
    // O nome comercial do processador não tem API dedicada no Win32; o caminho
    // canônico é a chave que o próprio sistema publica em HARDWARE\DESCRIPTION.
    HKEY key{};
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)", 0, KEY_READ,
                      &key) == ERROR_SUCCESS) {
        char name[256]{};
        DWORD size = static_cast<DWORD>(sizeof(name));
        DWORD type = 0;
        if (RegQueryValueExA(key, "ProcessorNameString", nullptr, &type,
                             reinterpret_cast<LPBYTE>(name), &size) == ERROR_SUCCESS &&
            type == REG_SZ) {
            info.cpu_model = trim(std::string{name});
        }
        RegCloseKey(key);
    }

    SYSTEM_INFO sys{};
    GetSystemInfo(&sys);
    info.cores_logical = static_cast<std::uint32_t>(sys.dwNumberOfProcessors);

    // Núcleos físicos: GetLogicalProcessorInformationEx conta RelationProcessorCore,
    // que é exatamente um registro por núcleo físico (SMT aparece como máscara).
    DWORD length = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length) == FALSE &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER && length > 0) {
        std::string buffer(length, '\0');
        auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, first, &length) != FALSE) {
            DWORD offset = 0;
            std::uint32_t physical = 0;
            while (offset < length) {
                auto* entry = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
                    buffer.data() + offset);
                if (entry->Size == 0) {
                    break;
                }
                if (entry->Relationship == RelationProcessorCore) {
                    ++physical;
                }
                offset += entry->Size;
            }
            info.cores_physical = physical;
        }
    }

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (GlobalMemoryStatusEx(&mem) != FALSE) {
        info.ram_bytes = static_cast<std::uint64_t>(mem.ullTotalPhys);
    }
#else
    if (FILE* cpuinfo = std::fopen("/proc/cpuinfo", "r"); cpuinfo != nullptr) {
        char line[512];
        while (std::fgets(line, sizeof(line), cpuinfo) != nullptr) {
            const std::string text{line};
            if (info.cpu_model.empty() && text.rfind("model name", 0) == 0) {
                if (const auto colon = text.find(':'); colon != std::string::npos) {
                    auto value = text.substr(colon + 1);
                    while (!value.empty() && value.front() == ' ') {
                        value.erase(value.begin());
                    }
                    info.cpu_model = trim(std::move(value));
                }
            }
        }
        std::fclose(cpuinfo);
    }

    if (const auto online = sysconf(_SC_NPROCESSORS_ONLN); online > 0) {
        info.cores_logical = static_cast<std::uint32_t>(online);
    }
    // Núcleos físicos em Linux exigiria deduplicar (physical id, core id) do
    // /proc/cpuinfo; enquanto não houver ambiente Linux calibrado, fica 0
    // (não coletado) em vez de repetir o valor lógico e mentir sobre SMT.

    if (const auto pages = sysconf(_SC_PHYS_PAGES); pages > 0) {
        if (const auto page = sysconf(_SC_PAGESIZE); page > 0) {
            info.ram_bytes = static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
        }
    }
#endif
}

} // namespace

std::string filesystem_name(std::string_view path) {
#if defined(_WIN32)
    // GetVolumeInformation quer a raiz do volume, não um caminho qualquer.
    std::string root{path};
    if (root.size() >= 2 && root[1] == ':') {
        root = root.substr(0, 2) + "\\";
    } else {
        return {};
    }
    char name[MAX_PATH + 1]{};
    if (GetVolumeInformationA(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, name,
                              static_cast<DWORD>(sizeof(name))) != FALSE) {
        return std::string{name};
    }
    return {};
#else
    // statfs devolve um f_type numérico cujo mapeamento para nome é uma tabela
    // à parte; /proc/mounts já traz o nome pronto. Escolhe o mount point mais
    // longo que prefixa `path` -- o mesmo critério que o kernel usa.
    FILE* mounts = std::fopen("/proc/mounts", "r");
    if (mounts == nullptr) {
        return {};
    }
    std::string best_point, best_type;
    char device[256], point[256], type[64];
    while (std::fscanf(mounts, "%255s %255s %63s %*[^\n]", device, point, type) == 3) {
        const std::string mount_point{point};
        if (path.rfind(mount_point, 0) == 0 && mount_point.size() >= best_point.size()) {
            best_point = mount_point;
            best_type = type;
        }
    }
    std::fclose(mounts);
    return best_type;
#endif
}

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

    // O nome vem do CMake; NDEBUG só distingue duas classes e colapsaria
    // RelWithDebInfo em "Release" (docs-process/PLANO_PROFILING.md §3, M4).
#if defined(MODB_BUILD_TYPE_NAME)
    info.build_type = MODB_BUILD_TYPE_NAME;
    if (info.build_type.empty()) {
        info.build_type = "unknown";
    }
#elif defined(NDEBUG)
    info.build_type = "Release";
#else
    info.build_type = "Debug";
#endif

#if defined(MODB_INSTRUMENTATION)
    info.instrumentation = MODB_INSTRUMENTATION;
#else
    info.instrumentation = "unknown";
#endif
    if (info.instrumentation.empty()) {
        info.instrumentation = "none";
    }

#if defined(_WIN32)
    info.os_name = "Windows";
    info.os_version = windows_version_string();
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

    collect_hardware(info);

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
