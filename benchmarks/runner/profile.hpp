#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace modb::bench {

struct ScenarioProfileOverride {
    std::string scenario_id;
    std::uint64_t object_count{0};
    std::uint64_t stride{1};
    std::size_t cache_pages{0}; // 0 = padrão do cenário
    int warmup{0};
    int samples{0};
};

struct CampaignProfile {
    std::string name;
    int default_warmup{3};
    int default_samples{10};
    std::uint64_t min_sample_ms{250};
    std::vector<ScenarioProfileOverride> scenarios;
};

[[nodiscard]] std::optional<CampaignProfile> find_profile(std::string_view name);
[[nodiscard]] std::vector<std::string> list_profile_names();

} // namespace modb::bench
