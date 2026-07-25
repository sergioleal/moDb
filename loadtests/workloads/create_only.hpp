#pragma once

// Workload `create_only` (docs/PLANO_TESTES_DE_CARGA.md §4.2): ingestão pura.
// Único workload com dispatch implementado nesta subfase (ver
// `matrix::is_workload_implemented`). Escolhe o alvo (só `embedded` por ora)
// e delega para o target correspondente -- o workload não sabe como o alvo
// executa, só monta os parâmetros e interpreta o resultado.

#include "matrix.hpp"
#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_create_only(const Case& c, const std::filesystem::path& work_dir,
                                            std::uint64_t seed, std::filesystem::path& out_db_path);

} // namespace modb::loadtest
