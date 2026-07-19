# API pública C++ — Fase 10E

Contrato estável instalado com `cmake --install` / `find_package(moDb CONFIG)`.
Alvo CMake: `modb::modb`.

## Headers instalados

| Header | Papel |
|---|---|
| `modb/error.hpp` | `Error` / `ErrorCode` / `Result` |
| `modb/version.hpp` | nome e versão do projeto |
| `modb/compatibility.hpp` | major/minor e negociação |
| `modb/limits.hpp` | tetos compartilhados |
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
| `modb/ops/facade_handle.hpp` | handle tipado e invoke embedded (11B) |

Dependências transitivas desses headers (ex.: `storage/page.hpp` puxado por
`database.hpp`) também são instaladas para o consumidor compilar.

## Não contrato (uso interno)

Ferramentas e testes in-tree podem incluir qualquer header sob `include/modb/`.
Consumidores externos devem restringir-se à lista acima; I/O nativo, WAL cru e
detalhes de página não são superfície estável.

## Exemplo consumidor

Ver `tests/consumer/` — cria/abre um banco só com headers instalados.
