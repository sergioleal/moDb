#pragma once

// Workload `restart_recovery` (§4.2.1): create -> churn com um commit
// interrompido de propósito -> reabre o banco (replay de WAL de verdade) ->
// verifica o estado pós-recuperação. Fora de todo perfil automático (§6.2)
// -- só roda via `--workload restart_recovery` explícito. Só `embedded`
// nesta subfase.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_restart_recovery(const Case& c, const std::filesystem::path& work_dir,
                                                 std::uint64_t seed,
                                                 const ProgressCallback& on_progress,
                                                 std::filesystem::path& out_db_path);

} // namespace modb::loadtest
