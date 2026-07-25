#pragma once

// Workload `create_only` (docs/PLANO_TESTES_DE_CARGA.md §4.2): ingestão pura.
// Único workload com dispatch para `embedded` E `loopback` (Subfase G,
// versão mínima -- os demais workloads continuam só `embedded`). Escolhe o
// alvo e delega para o target correspondente -- o workload não sabe como o
// alvo executa, só monta os parâmetros e interpreta o resultado.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_create_only(const Case& c, const std::filesystem::path& work_dir,
                                            std::uint64_t seed, const ProgressCallback& on_progress,
                                            std::filesystem::path& out_db_path);

} // namespace modb::loadtest
