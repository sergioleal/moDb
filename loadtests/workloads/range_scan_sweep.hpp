#pragma once

// Workload `range_scan_sweep` (§4.2.1): create -> índice em `User.id` ->
// scans por faixa em seletividades de 0,01% a 100%. Formaliza o antigo
// `crud_query` do plano de benchmarks -- só `embedded` nesta subfase.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_range_scan_sweep(const Case& c,
                                                 const std::filesystem::path& work_dir,
                                                 std::uint64_t seed,
                                                 const ProgressCallback& on_progress,
                                                 std::filesystem::path& out_db_path);

} // namespace modb::loadtest
