#pragma once

// Módulo de exemplo do servidor de aplicação (PLANO_SERVIDOR_PROCS S1.3): um
// tipo (Nota) e duas procs. Compilado dentro de `notas-server` por
// modb_add_server; o cliente só chama as procs pelo nome.
//
//   notas.criar  (escrita)  args: texto UTF-8        -> id da nota (u64)
//   notas.ler    (leitura)  args: id da nota (u64)   -> texto UTF-8
//
// Os argumentos ainda são bytes montados à mão; a S2 troca por valores
// autodescritos.

#include "modb/server/host.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modb::examples::notas {

inline constexpr std::string_view k_criar = "notas.criar";
inline constexpr std::string_view k_ler = "notas.ler";

struct Nota {
    std::string texto;
};

// Codificação dos argumentos e resultados, usada pelo módulo e pelo cliente.
std::vector<std::byte> texto_para_bytes(std::string_view texto);
std::string bytes_para_texto(std::span<const std::byte> bytes);
std::vector<std::byte> id_para_bytes(std::uint64_t id);
Result<std::uint64_t> bytes_para_id(std::span<const std::byte> bytes);

} // namespace modb::examples::notas

// Ponto de entrada do módulo, procurado por modb_add_server(... MODULES notas_procs).
modb::server::Module modb_module_notas_procs();
