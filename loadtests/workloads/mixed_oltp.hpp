#pragma once

// Workload `mixed_oltp` (§4.2.1): sessões concorrentes de verdade emitindo
// create/read/update/delete (5/80/10/5) contra o mesmo banco -- único
// workload que lê `c.concurrency` (fecha a dívida D1 para essa dimensão).
// Só `embedded` nesta subfase.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_mixed_oltp(const Case& c, const std::filesystem::path& work_dir,
                                           std::uint64_t seed, const ProgressCallback& on_progress,
                                           std::filesystem::path& out_db_path);

} // namespace modb::loadtest
