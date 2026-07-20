# Ring0

Banco **Orientado a Objetos** embutido, persistente em arquivo, em C++26.
Identidade estável (`ObjectId`), tipos versionados, relacionamentos, coleções,
transações com WAL, consultas com streaming e operações de domínio no processo.

O repositório, o namespace, a CLI e o pacote CMake ainda usam o identificador
técnico `modb` / `moDb`.

## Começar em 5 minutos

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

# Demo OO (cria, evolui schema, consulta)
.\build\debug\modb.exe oo employee demo demo.modb --force

# Diagnóstico
.\build\debug\modb.exe db check demo.modb
```

Fluxo mínimo com a API C++ (após `cmake --install` / `find_package(moDb)`):

```cpp
#include <modb/object/database.hpp>

auto db = modb::object::Database::create("shop.modb");
// bind tipos, criar objetos, commit — ver docs/API_PUBLICA.md
```

## O que o motor oferece

| Área | Entrega |
|---|---|
| Objetos + catálogo | tipos, baselines, Binding, ProjectionPlan |
| Persistência | páginas, TableHeap, BlobStore, BufferPool |
| Durabilidade | WAL + recovery (`<db>.wal`) |
| Consultas | streaming, índices B-tree, planner |
| Rede | protocolo binário Hello…, cliente/servidor |
| Operações | `Operation` / `ExecutionContext` no processo |
| Robustez | sanitizers, fuzz (10D), compat major/minor (10E) |

## Documentação

| Documento | Uso |
|---|---|
| [docs/RASTREADOR.md](docs/RASTREADOR.md) | estado das fases |
| [docs/PLANO_ODB.md](docs/PLANO_ODB.md) | plano Ring0 |
| [docs/PROTOCOLO_FASES.md](docs/PROTOCOLO_FASES.md) | critérios por fase |
| [docs/FORMATO_DE_ARQUIVO.md](docs/FORMATO_DE_ARQUIVO.md) | layout em disco |
| [docs/API_PUBLICA.md](docs/API_PUBLICA.md) | headers instaláveis |
| [docs/COMPATIBILIDADE.md](docs/COMPATIBILIDADE.md) | major/minor |
| [docs/OPERACAO.md](docs/OPERACAO.md) | backup, restore, supervisor, `db check` |
| [docs/BASELINE_DESEMPENHO.md](docs/BASELINE_DESEMPENHO.md) | runner 10A |
| [docs/FECHAMENTO_10F.md](docs/FECHAMENTO_10F.md) | matriz final Fase 10 |
| [arquitetura.md](arquitetura.md) | modelo de objetos |
| [codigo-local.md](codigo-local.md) | operações no servidor |
| [streaming.md](streaming.md) | modelo de consulta |

## Build

Requisitos: CMake ≥ 3.30, Ninja, compilador C++26 (fallback C++23 local opcional).

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

Presets úteis: `release`, `fuzz`, `profile-8k`, `profile-16k`, `local-gcc13`.
Instalação da biblioteca: `cmake --install build/debug --prefix <prefix>` —
detalhes em [API_PUBLICA.md](docs/API_PUBLICA.md).

## CLI (amostra)

```text
modb oo employee demo <file> [--force]
modb db check <file>
modb tx demo <file> [--force]
modb tx wal-info <file>
modb serve demo <file> [--force]
modb_bench run --profile smoke --seed 1 --output-dir benchmark-results
```

Backup quiescente: parar escritores e copiar `<db>` **e** `<db>.wal` juntos —
procedimento em [OPERACAO.md](docs/OPERACAO.md).

## Licença / status

Projeto em desenvolvimento ativo. Formato e protocolo seguem a política
major/minor da Fase 10E; a superfície C++ instalável está documentada na 10E.
