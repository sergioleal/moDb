#pragma once

// Módulo de exemplo do servidor de aplicação (PLANO_SERVIDOR_PROCS S1–S4): um
// tipo (Nota), um índice e procs declaradas como funções. Compilado dentro de
// `notas-server` por modb_add_server; o cliente só chama as procs pelo nome.
//
//   notas.criar     (escrita)  {texto}            -> {id}     conflito se o texto já existe
//   notas.ler       (leitura)  {id}               -> {id, texto}
//   notas.listar    (leitura)  {contem?}          -> [{id, texto}...]
//   notas.editar    (escrita)  {id, texto}        -> {id, texto}
//   notas.apagar    (escrita)  {id}               -> {}
//   notas.excecao   (escrita)  {texto}            -> cria e lança exceção: prova o rollback
//
// Argumentos e resultados são ops::Value (binário no fio; JSON no `modb call`).
//
// Configuração (ModuleBuilder::setting): `notas.max_texto = N` no `.conf`, ou
// `--notas.max-texto N`; o tamanho máximo do texto em bytes (padrão 1000).

#include "modb/server/module.hpp"

#include <string>

namespace modb::examples::notas {

struct Nota {
    std::string texto;
};

inline constexpr object::FieldId k_texto{1};

} // namespace modb::examples::notas

// Ponto de entrada do módulo, procurado por modb_add_server(... MODULES notas_procs).
modb::server::Module modb_module_notas_procs();
