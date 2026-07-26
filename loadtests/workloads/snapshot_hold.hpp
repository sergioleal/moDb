#pragma once

// Workload `snapshot_hold` (§4.2.1): create -> abre snapshot -> churn ->
// relê pela mesma snapshot ainda aberta -> fecha -> GC -> relê sem
// snapshot. Só `embedded` nesta subfase.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_snapshot_hold(const Case& c, const std::filesystem::path& work_dir,
                                              std::uint64_t seed, const ProgressCallback& on_progress,
                                              std::filesystem::path& out_db_path);

} // namespace modb::loadtest
