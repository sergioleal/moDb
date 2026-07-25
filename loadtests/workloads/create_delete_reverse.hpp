#pragma once

// Workload `create_delete_reverse` (§4.2): create → delete em ordem inversa
// (LIFO) -- estressa compactação da última página e o caminho de
// encolhimento.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_create_delete_reverse(const Case& c,
                                                      const std::filesystem::path& work_dir,
                                                      std::uint64_t seed,
                                                      std::filesystem::path& out_db_path);

} // namespace modb::loadtest
