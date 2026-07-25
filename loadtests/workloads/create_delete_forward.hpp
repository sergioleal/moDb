#pragma once

// Workload `create_delete_forward` (§4.2): create → delete na mesma ordem de
// criação (FIFO) -- estressa remoção com localidade perfeita e devolução de
// espaço em ordem.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_create_delete_forward(const Case& c,
                                                      const std::filesystem::path& work_dir,
                                                      std::uint64_t seed,
                                                      std::filesystem::path& out_db_path);

} // namespace modb::loadtest
