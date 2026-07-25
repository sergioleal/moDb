#pragma once

// Alvo `embedded` (§4.3): motor in-process, sem rede. Implementação real
// contra `modb::object::Database` -- não é um stub.

#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

// Cria um banco novo em `params.work_dir`, faz bind do tipo `User`, cria
// `params.object_count` objetos em lotes de `params.batch` por commit, e lê
// todos de volta para validar o hash lógico (§9: "hash lógico do conjunto
// confere com o esperado do dataset"). Deixa o arquivo do banco em disco (o
// chamador decide se remove) e devolve, em `out_db_path`, o caminho usado.
[[nodiscard]] CaseRunResult run_create_only_embedded(const WorkloadParams& params,
                                                     std::filesystem::path& out_db_path);

} // namespace modb::loadtest
