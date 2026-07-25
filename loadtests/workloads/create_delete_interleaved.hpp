#pragma once

// Workload `create_delete_interleaved` (§4.2): create → delete por stride
// -- estressa fragmentação real, free-list e reuso parcial de espaço.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_create_delete_interleaved(const Case& c,
                                                          const std::filesystem::path& work_dir,
                                                          std::uint64_t seed,
                                                          std::filesystem::path& out_db_path);

} // namespace modb::loadtest
