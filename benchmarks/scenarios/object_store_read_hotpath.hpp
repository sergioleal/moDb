#pragma once

#include "scenarios/object_store_lifecycle.hpp"

#include <cstdint>
#include <string>

namespace modb::bench {

struct ReadHotpathParams {
    std::uint64_t seed{0};
    std::uint64_t object_count{0};
    std::uint64_t read_rounds{0};
    std::string work_dir;
};

// Cria N objetos e mede get+materialize em várias passadas (caminho quente 10C).
[[nodiscard]] SampleResult run_object_store_read_hotpath(const ReadHotpathParams& params);

} // namespace modb::bench
