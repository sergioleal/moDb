# Database Lifecycle

> **Status:** 🚧 skeleton — outline only, not yet written. In the meantime see
> [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapters 1 and 3.

## Overview

- TODO: `create` vs `open`, the on-disk file(s) involved (`<db>`, `<db>.wal`),
  move-only `Database`.
- TODO: why every `Handle`/`Transaction`/`Snapshot` requires the owning
  `Database` to be `shared_ptr`-wrapped and attached to `DatabaseRegistry`.

## Core API surface

- TODO `Database::create(path, cache_capacity = default)` —
  `include/modb/object/database.hpp:378-381`
- TODO `Database::create(path, DatabaseOptions, cache_capacity)` —
  `database.hpp:382-383`
- TODO `Database::open(path, cache_capacity)` / `open(path, DatabaseOptions, cache_capacity)`
  — `database.hpp:384-390`
- TODO `DatabaseRegistry::attach/find/detach/instance()` —
  `database.hpp:345-356`
- TODO `DatabaseOptions` — `include/modb/object/primary_storage.hpp:24-28`
  - `PrimaryStorage{full, wal_only}` — `primary_storage.hpp:13-16`
  - `CommitAckPolicy{local_wal, await_one_replica}` — `primary_storage.hpp:19-22`
  - `WalIoMode{sync, async}` — `primary_storage.hpp` (Phase 13)
- TODO instance control file (`MCTL`) for `wal_only` primaries —
  `include/modb/object/instance_control.hpp`

## Semantics & invariants

- TODO: `create` fails if the file exists; `open` fails if it doesn't.
- TODO: `Handle<T>`/`Transaction`/`Snapshot` only store a `DatabaseId`,
  resolved via `DatabaseRegistry::find()` on every call — consequence for
  lifetime management.
- TODO: default `DatabaseOptions` (`primary_storage=full`, `commit_ack=local_wal`,
  `wal_io=sync`) — no implicit upgrade to `wal_only`/`async`/`await_one_replica`.

## Common pitfalls

- TODO: forgetting `detach()` leaks the registry slot (keeps the `Database`
  alive).
- TODO: using a `Handle` after its `Database` was detached.

## Related documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapters 1, 3
- [OPERACAO_REPLICACAO.md](../OPERACAO_REPLICACAO.md) — `wal_only` primaries
  in the replication context
- [OPERACAO_IO_ASSINCRONO.md](../OPERACAO_IO_ASSINCRONO.md) — `WalIoMode`
- [ADR-017](../decisions/ADR-017-primary-wal-only-sem-arquivos-de-dados.md)

## Related source

- `include/modb/object/database.hpp`, `primary_storage.hpp`, `instance_control.hpp`
- `examples/server/by_phase/phase_02/persist_reopen.cpp`
- `tests/consumer/main.cpp` — smallest possible external consumer
