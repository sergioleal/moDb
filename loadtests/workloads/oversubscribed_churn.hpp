#pragma once

// Workload `oversubscribed_churn` (§4.2.1): create -> delete intercalado
// (mesma ordem de create_delete_interleaved) com o buffer pool
// explicitamente menor que o working set. Só `embedded` nesta subfase.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_oversubscribed_churn(const Case& c,
                                                     const std::filesystem::path& work_dir,
                                                     std::uint64_t seed,
                                                     const ProgressCallback& on_progress,
                                                     std::filesystem::path& out_db_path);

} // namespace modb::loadtest
