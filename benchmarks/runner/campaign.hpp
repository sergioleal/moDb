#pragma once

#include "runner/environment.hpp"
#include "runner/profile.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace modb::bench {

struct CampaignOptions {
    std::string profile{"smoke"};
    std::uint64_t seed{1};
    std::filesystem::path output_dir{"benchmark-results"};
    std::filesystem::path work_dir;
    std::string filter; // substring match on scenario_id; empty = all
    std::string argv_joined;
};

struct CampaignResult {
    bool ok{false};
    std::string status; // completed | failed | partial
    std::filesystem::path result_path;
    std::string run_id;
    std::string error;
};

[[nodiscard]] CampaignResult run_campaign(const CampaignOptions& options);

struct CompareScenarioDelta {
    std::string scenario_id;
    std::string parameters_key;
    bool compatible{false};
    double baseline_ops_per_sec{0.0};
    double candidate_ops_per_sec{0.0};
    double delta_percent{0.0};
    std::string verdict; // ok | alert | fail | incompatible
};

struct CompareResult {
    bool ok{false};
    std::string error;
    std::vector<CompareScenarioDelta> deltas;
};

[[nodiscard]] CompareResult compare_campaigns(const std::filesystem::path& baseline,
                                              const std::filesystem::path& candidate);

} // namespace modb::bench
