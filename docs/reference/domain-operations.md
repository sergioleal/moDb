# Remote Domain Operations

> **Status:** 🚧 skeleton — outline only, not yet written. In the meantime see
> [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 10, and
> [codigo-local.md](../../codigo-local.md) for the vision-level rationale.

## Overview

- TODO: running business logic in the server process, callable by id, next
  to the data (vs. shipping a query to the client).
- TODO: the admission model (`ModuleManifest` hash allowlist) as a
  production-style guard against loading unexpected code.

## Core API surface

- TODO `Operation` interface (`id()`, `mode()`, `execute(ExecutionContext&)`)
  — `include/modb/ops/operation.hpp:29-46`
- TODO `OperationRegistry::register_factory/register_operation/dispatch` —
  `include/modb/ops/operation_registry.hpp:17-53`
- TODO `ExecutionContext` — `include/modb/ops/execution_context.hpp`
- TODO `ModuleManifest`, `ModuleLoader::admit_hash/load` —
  `include/modb/ops/module_manifest.hpp:34-78`
- TODO `OperationMode{read_only, read_write}`

## Semantics & invariants

- TODO: `dispatch()` runs begin → `execute()` → commit, rolling back on any
  error **or thrown exception**.
- TODO: arguments are hand-rolled binary encode/decode
  (`storage::BinaryWriter`/`BinaryReader`), not reflection-based.
- TODO: manifest `hash` covers exported methods and facades (see
  [FACADES.md](../FACADES.md)) — changing the surface requires a new hash in
  the allowlist.

## Common pitfalls

- TODO: validating business rules *after* mutating state instead of before.
- TODO: forgetting that rollback undoes the transaction, not manual
  in-memory state kept elsewhere.

## Related documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 10
- [FACADES.md](../FACADES.md) — the versioned surface layered on top of
  operations
- [OPERACAO_MODULOS.md](../OPERACAO_MODULOS.md) — failure model of the module
  runtime
- [ADR-012](../decisions/ADR-012-runtime-de-modulos-no-processo.md)

## Related source

- `include/modb/ops/operation.hpp`, `operation_registry.hpp`,
  `execution_context.hpp`, `module_manifest.hpp`
- `examples/transfer_funds/`
- `examples/server/by_phase/phase_09/call_operation.cpp`
- `tests/operation_test.cpp`, `tests/operation_server_test.cpp`
