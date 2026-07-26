#pragma once

// Workload `blob_lifecycle` (§4.2.1): create -> read/stream -> grow ->
// shrink -> delete sobre `BlobStore`. Só `embedded` nesta subfase.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_blob_lifecycle(const Case& c, const std::filesystem::path& work_dir,
                                               std::uint64_t seed,
                                               const ProgressCallback& on_progress,
                                               std::filesystem::path& out_db_path);

} // namespace modb::loadtest
