#pragma once

// Alvo `loopback` (Subfase G, docs/PLANO_TESTES_DE_CARGA.md §4.3): mesmo
// motor de `target_embedded.cpp`, mas criado e lido através de um
// `modb::net::Server`/`Client` de verdade em 127.0.0.1 -- não um objeto
// simulado. Versão mínima desta subfase: só `create_only`. Ver
// `loadtest_facade.hpp` para a operação de rede usada e
// docs-process/PLANO_IMPLEMENTACAO_CARGA.md para o que fica de fora.
//
// `via_proxy` (alvo `loopback_proxy`, ADR-028/PLANO_PROXY X10): o engine
// escuta só no link local e o cliente fala com um `modb::proxy::Proxy` de
// passagem no mesmo processo -- a diferença para `loopback` é o salto a mais.

#include "target.hpp"

#include <filesystem>

namespace modb::loadtest {

[[nodiscard]] CaseRunResult run_create_only_client(const WorkloadParams& params,
                                                   std::filesystem::path& out_db_path, bool via_proxy = false);

} // namespace modb::loadtest
