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

// crud_full (§4.2): create -> read -> update_inplace -> update_grow ->
// update_shrink -> delete, seis PhaseMetrics separados. Cada update valida
// uma amostra determinística campo a campo (§9 item 4), não o conjunto
// inteiro.
[[nodiscard]] CaseRunResult run_crud_full_embedded(const WorkloadParams& params,
                                                   std::filesystem::path& out_db_path);

// read_hotspot (§4.2.1): create → leituras enviesadas por Zipf sobre o
// working set fixo. Valida os valores lidos contra o esperado (na mesma
// ordem em que foram lidos) e mede a taxa de acerto do buffer pool durante
// a fase de leitura.
[[nodiscard]] CaseRunResult run_read_hotspot_embedded(const WorkloadParams& params,
                                                      std::filesystem::path& out_db_path);

// range_scan_sweep (§4.2.1): create → índice em `User.id` → uma fase de
// scan por faixa para cada seletividade de 0,01% a 100%. Cada fase registra
// no próprio nome se o plano usou índice ou table scan (`query::QueryPlan`).
[[nodiscard]] CaseRunResult run_range_scan_sweep_embedded(const WorkloadParams& params,
                                                          std::filesystem::path& out_db_path);

// mixed_oltp (§4.2.1): cria um working set inicial, depois `params.concurrency`
// threads emitindo create/read/update/delete (proporção fixa 5/80/10/5) contra
// o MESMO banco, serializadas por um mutex (o motor é single-thread, ADR-011)
// -- contenção real na fila de entrada, não paralelismo real dentro do motor.
// Fecha a dívida D1 para `--concurrency` (docs-process/PLANO_IMPLEMENTACAO_CARGA.md).
[[nodiscard]] CaseRunResult run_mixed_oltp_embedded(const WorkloadParams& params,
                                                    std::filesystem::path& out_db_path);

// snapshot_hold (§4.2.1): create -> abre uma snapshot -> churn (update numa
// metade dos ids, delete em outra, cria alguns novos) -> relê pela MESMA
// snapshot ainda aberta (deve bater byte a byte com o estado da abertura) ->
// fecha a snapshot -> `collect_garbage()` -> relê sem snapshot (deve refletir
// o churn). Cada id é tocado no máximo uma vez durante o churn -- uma
// segunda alteração no mesmo objeto com a snapshot ainda aberta falharia com
// `snapshot_conflict` (só há espaço para uma versão `previous` por vez).
[[nodiscard]] CaseRunResult run_snapshot_hold_embedded(const WorkloadParams& params,
                                                       std::filesystem::path& out_db_path);

} // namespace modb::loadtest
