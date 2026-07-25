#pragma once

// Workload `crud_full` (§4.2): ciclo completo -- create, read,
// update_inplace, update_grow, update_shrink, delete. Seis fases separadas,
// cada uma com seu próprio phase_summary.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_crud_full(const Case& c, const std::filesystem::path& work_dir,
                                          std::uint64_t seed, std::filesystem::path& out_db_path);

} // namespace modb::loadtest
