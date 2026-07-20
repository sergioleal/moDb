# API pública C++ — Fase 10E

Contrato estável do Ring0 instalado com `cmake --install` /
`find_package(moDb CONFIG)`.
Alvos CMake:

- `modb::modb`: motor, protocolo e servidor.
- `modb::app_client`: biblioteca recomendada para aplicações que conectam ao
  servidor Ring0.

## Headers instalados

| Header | Papel |
|---|---|
| `modb/error.hpp` | `Error` / `ErrorCode` / `Result` |
| `modb/version.hpp` | nome e versão do projeto |
| `modb/compatibility.hpp` | major/minor e negociação |
| `modb/limits.hpp` | tetos compartilhados |
| `modb/app/server_connection.hpp` | conexão de aplicação ao servidor (`modb::app_client`) |
| `modb/object/database.hpp` | fachada OO principal |
| `modb/object/ids.hpp` | identidades |
| `modb/object/attribute_value.hpp` | valores |
| `modb/object/type_definition.hpp` | tipos |
| `modb/object/binding.hpp` | binding C++ ↔ campos |
| `modb/object/baseline.hpp` | baselines |
| `modb/object/collection.hpp` | coleções persistentes |
| `modb/object/ref.hpp` / `handle.hpp` | referências e handles |
| `modb/net/client.hpp` | cliente de protocolo |
| `modb/net/protocol.hpp` | mensagens e codec de frames |
| `modb/net/query_description.hpp` | descrição de query |
| `modb/ops/operation.hpp` | operações de domínio |
| `modb/ops/execution_context.hpp` | contexto de execução |
| `modb/ops/operation_registry.hpp` | registro |
| `modb/ops/module_manifest.hpp` | manifesto de módulo |
| `modb/ops/facade_descriptor.hpp` | descritores de facade/método (11A) |
| `modb/ops/facade_catalog.hpp` | catálogo de facades (11A) |
| `modb/ops/facade_handle.hpp` | handle tipado embedded/remoto (11B/11D) |
| `modb/graph/edge_handle.hpp` | arestas tipadas runtime `Ref`/`OwnedRef` (12A) |
| `modb/graph/graph_view.hpp` | adjacência de coleção e incoming indexado (12B) |
| `modb/graph/traversal.hpp` | BFS/DFS lazy com limites e cancel (12C) |
| `modb/graph/algorithms.hpp` | Caminho mínimo, ciclo, toposort, componentes (12D) |
| `modb/storage/async_file.hpp` | I/O assíncrono posicional com backpressure (13) |

Contrato de facades (manifesto → catálogo → handle → registry):
[FACADES.md](FACADES.md).

Dependências transitivas desses headers (ex.: `storage/page.hpp` puxado por
`database.hpp`) também são instaladas para o consumidor compilar.

## Não contrato (uso interno)

Ferramentas e testes in-tree podem incluir qualquer header sob `include/modb/`.
Consumidores externos devem restringir-se à lista acima; I/O nativo, WAL cru e
detalhes de página não são superfície estável.

## Exemplo consumidor

Ver `tests/consumer/` — cria/abre um banco só com headers instalados.

## Exemplos de servidor por fase

Ver `examples/server/by_phase/`. Ha um exemplo por fase de 00 a 13; a partir da
Fase 8 eles mostram aplicacoes conectando ao servidor com
`modb::app::ServerConnection`.
