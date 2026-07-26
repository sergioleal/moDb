#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace modb::bench {

struct EnvironmentInfo {
    std::string git_commit;
    std::string git_commit_short;
    std::string git_branch;
    bool git_dirty{false};
    std::string compiler_id;
    std::string compiler_version;
    std::string cxx_standard;
    // Nome real do CMAKE_BUILD_TYPE ("Debug", "RelWithDebInfo", "Release", ...).
    // Antes era derivado de NDEBUG, o que colapsava RelWithDebInfo em "Release"
    // e escondia justamente a distinção que o profiling precisa fazer
    // (docs-process/PLANO_PROFILING.md §3, M4).
    std::string build_type;
    // Instrumentação ativa que altera o desempenho: "none", "gprof",
    // "coverage", "sanitizers" ou uma combinação separada por '+'. Entra na
    // series_key: uma corrida com -pg é 2-3x mais lenta e nunca deve cair na
    // mesma série de uma corrida limpa.
    std::string instrumentation;
    std::string os_name;
    std::string os_version;
    std::string arch;
    std::string cpu_model;
    // 0 quando não foi possível determinar (nunca um valor inventado).
    std::uint32_t cores_physical{0};
    std::uint32_t cores_logical{0};
    std::uint64_t ram_bytes{0};
    std::string hostname_token;
    std::string page_size;
    std::string project_version;
    std::string argv_joined;
};

// Nome do sistema de arquivos que contém `path` ("NTFS", "ext4", ...).
// Vazio quando indisponível. Depende do diretório de trabalho, por isso é
// consultado à parte de collect_environment().
[[nodiscard]] std::string filesystem_name(std::string_view path);

// Coleta metadados disponíveis sem falhar a campanha se algum item faltar.
[[nodiscard]] EnvironmentInfo collect_environment(std::string_view argv_joined);

// Instantâneo UTC com milissegundos: YYYYMMDDTHHMMSS.mmmZ
[[nodiscard]] std::string utc_timestamp_millis();

// Gera um run_id estável o bastante para distinguir campanhas.
[[nodiscard]] std::string make_run_id(std::string_view utc_stamp);

} // namespace modb::bench
