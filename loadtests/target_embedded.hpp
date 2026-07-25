#pragma once

// Alvo `embedded` (§4.3): motor in-process, sem rede. Implementação real
// contra `modb::object::Database` -- não é um stub.

#include "target.hpp"

#include <filesystem>
#include <string_view>

namespace modb::loadtest {

// Cria um banco novo em `params.work_dir`, faz bind do tipo `User`, cria
// `params.object_count` objetos em lotes de `params.batch` por commit, e lê
// todos de volta para validar o hash lógico (§9: "hash lógico do conjunto
// confere com o esperado do dataset"). Deixa o arquivo do banco em disco (o
// chamador decide se remove) e devolve, em `out_db_path`, o caminho usado.
[[nodiscard]] CaseRunResult run_create_only_embedded(const WorkloadParams& params,
                                                     std::filesystem::path& out_db_path);

// Ordem de remoção (§4.2) -- decide o que cada workload de create/delete
// estressa: localidade perfeita (Forward), caminho de encolhimento
// (Reverse), ou fragmentação/acesso disperso (Interleaved).
enum class DeleteOrder { Forward, Reverse, Interleaved };

// create → delete (na ordem de `order`) sobre o mesmo banco, seguido da
// validação de que nenhum objeto removido continua resolvendo (§9).
// `workload_tag` só nomeia o arquivo .modb gerado (diagnóstico).
[[nodiscard]] CaseRunResult run_create_delete_embedded(const WorkloadParams& params,
                                                       DeleteOrder order,
                                                       std::string_view workload_tag,
                                                       std::filesystem::path& out_db_path);

} // namespace modb::loadtest
