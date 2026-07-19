#pragma once

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
    std::string build_type;
    std::string os_name;
    std::string os_version;
    std::string arch;
    std::string hostname_token;
    std::string page_size;
    std::string project_version;
    std::string argv_joined;
};

// Coleta metadados disponíveis sem falhar a campanha se algum item faltar.
[[nodiscard]] EnvironmentInfo collect_environment(std::string_view argv_joined);

// Instantâneo UTC com milissegundos: YYYYMMDDTHHMMSS.mmmZ
[[nodiscard]] std::string utc_timestamp_millis();

// Gera um run_id estável o bastante para distinguir campanhas.
[[nodiscard]] std::string make_run_id(std::string_view utc_stamp);

} // namespace modb::bench
