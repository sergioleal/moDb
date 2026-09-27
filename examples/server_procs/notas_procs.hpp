#pragma once

// Módulo de exemplo do servidor de aplicação (PLANO_SERVIDOR_PROCS S1.3/S2): um
// tipo (Nota) e duas procs. Compilado dentro de `notas-server` por
// modb_add_server; o cliente só chama as procs pelo nome.
//
//   notas.criar  (escrita)  {"texto": "..."}  -> {"id": <id>}
//   notas.ler    (leitura)  {"id": <id>}      -> {"id": <id>, "texto": "..."}
//
// Argumentos e resultados são ops::Value (codificação binária no fio; JSON em
// gateways e no CLI).

#include "modb/ops/value.hpp"
#include "modb/server/host.hpp"

#include <string>
#include <string_view>

namespace modb::examples::notas {

inline constexpr std::string_view k_criar = "notas.criar";
inline constexpr std::string_view k_ler = "notas.ler";

struct Nota {
    std::string texto;
};

} // namespace modb::examples::notas

// Ponto de entrada do módulo, procurado por modb_add_server(... MODULES notas_procs).
modb::server::Module modb_module_notas_procs();
