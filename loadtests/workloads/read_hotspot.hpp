#pragma once

// Workload `read_hotspot` (§4.2.1): create -> leituras enviesadas por Zipf
// sobre o working set fixo. Isola pressão de buffer pool/page cache sob
// leitura enviesada -- só `embedded` nesta subfase.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_read_hotspot(const Case& c, const std::filesystem::path& work_dir,
                                             std::uint64_t seed, const ProgressCallback& on_progress,
                                             std::filesystem::path& out_db_path);

} // namespace modb::loadtest
