#pragma once

// Workload `cascade_delete` (§4.2.1): create_hierarchy (profundidade x
// largura) -> cascade_delete (raiz). Só `embedded` nesta subfase.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_cascade_delete(const Case& c, const std::filesystem::path& work_dir,
                                               std::uint64_t seed,
                                               const ProgressCallback& on_progress,
                                               std::filesystem::path& out_db_path);

} // namespace modb::loadtest
