#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace modb::bench {

struct SampleMetrics {
    std::uint64_t operations{0};
    std::uint64_t elapsed_ns{0};
    std::uint64_t create_ns{0};
    std::uint64_t create_flush_ns{0};
    std::uint64_t delete_ns{0};
    std::uint64_t delete_flush_ns{0};
    std::uint64_t peak_file_bytes{0};
    double objects_per_second{0.0};
};

struct SampleResult {
    bool valid{true};
    bool comparable{true};
    std::string error;
    SampleMetrics metrics;
};

struct ScenarioParams {
    std::string scenario_id;
    std::uint64_t seed{0};
    std::uint64_t object_count{0};
    std::uint64_t stride{1};
    std::string cache_state{"warm"};
    std::string work_dir;
};

// Executa uma repetição do cenário object_store.lifecycle.
[[nodiscard]] SampleResult run_object_store_lifecycle(const ScenarioParams& params);

} // namespace modb::bench
