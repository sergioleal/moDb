#include "runner/profile.hpp"

namespace modb::bench {

std::optional<CampaignProfile> find_profile(std::string_view name) {
    if (name == "smoke") {
        CampaignProfile profile;
        profile.name = "smoke";
        profile.default_warmup = 1;
        profile.default_samples = 3;
        profile.min_sample_ms = 50;
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "object_store.lifecycle",
            .object_count = 200,
            .stride = 1,
            .warmup = 1,
            .samples = 3,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "object_store.lifecycle",
            .object_count = 200,
            .stride = 7,
            .warmup = 1,
            .samples = 3,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "storage.buffer_pool.oversubscribed",
            .object_count = 120,
            .cache_pages = 8,
            .warmup = 1,
            .samples = 2,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "object_store.read_hotpath",
            .object_count = 100,
            .read_rounds = 5,
            .warmup = 1,
            .samples = 3,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "graph.traversal.warm",
            .object_count = 3, // depth
            .stride = 2,       // branching
            .warmup = 1,
            .samples = 2,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "graph.traversal.cold",
            .object_count = 3,
            .stride = 2,
            .warmup = 0,
            .samples = 2,
        });
        return profile;
    }
    if (name == "standard") {
        CampaignProfile profile;
        profile.name = "standard";
        profile.default_warmup = 3;
        profile.default_samples = 10;
        profile.min_sample_ms = 250;
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "object_store.lifecycle",
            .object_count = 10'000,
            .stride = 1,
            .warmup = 3,
            .samples = 10,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "object_store.lifecycle",
            .object_count = 10'000,
            .stride = 17,
            .warmup = 3,
            .samples = 10,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "storage.buffer_pool.oversubscribed",
            .object_count = 2'000,
            .cache_pages = 16,
            .warmup = 2,
            .samples = 8,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "object_store.read_hotpath",
            .object_count = 1'000,
            .read_rounds = 20,
            .warmup = 2,
            .samples = 8,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "graph.traversal.warm",
            .object_count = 5,
            .stride = 3,
            .warmup = 2,
            .samples = 5,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "graph.traversal.cold",
            .object_count = 5,
            .stride = 3,
            .warmup = 1,
            .samples = 5,
        });
        return profile;
    }
    if (name == "diagnostic") {
        CampaignProfile profile;
        profile.name = "diagnostic";
        profile.default_warmup = 0;
        profile.default_samples = 2;
        profile.min_sample_ms = 1;
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "object_store.lifecycle",
            .object_count = 50,
            .stride = 1,
            .warmup = 0,
            .samples = 2,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "storage.buffer_pool.oversubscribed",
            .object_count = 80,
            .cache_pages = 4,
            .warmup = 0,
            .samples = 1,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "object_store.read_hotpath",
            .object_count = 40,
            .read_rounds = 3,
            .warmup = 0,
            .samples = 1,
        });
        profile.scenarios.push_back(ScenarioProfileOverride{
            .scenario_id = "graph.traversal.warm",
            .object_count = 2,
            .stride = 2,
            .warmup = 0,
            .samples = 1,
        });
        return profile;
    }
    return std::nullopt;
}

std::vector<std::string> list_profile_names() {
    return {"smoke", "standard", "diagnostic"};
}

} // namespace modb::bench
