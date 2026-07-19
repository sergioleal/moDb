#pragma once

#include "scenarios/object_store_lifecycle.hpp"

#include <cstdint>
#include <string>

namespace modb::bench {

struct BufferPoolParams {
    std::uint64_t seed{0};
    std::uint64_t object_count{0};
    std::size_t cache_pages{8};
    std::string work_dir;
};

// Working set maior que o cache: create + full read-back sob eviction.
[[nodiscard]] SampleResult run_buffer_pool_oversubscribed(const BufferPoolParams& params);

} // namespace modb::bench
