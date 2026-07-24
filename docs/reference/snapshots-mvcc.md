# Snapshots and MVCC

> **Status:** 🚧 skeleton — outline only, not yet written. In the meantime see
> [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 7, and
> [GARANTIAS_TRANSACIONAIS.md](../GARANTIAS_TRANSACIONAIS.md) §8-9 for the
> underlying guarantees.

## Overview

- TODO: a `Snapshot` fixes an epoch, not a copy of the data; how reads at a
  snapshot differ from a live `Handle<T>` read.
- TODO: why garbage collection is explicit, not automatic.

## Core API surface

- TODO `Database::snapshot()` → `Result<Snapshot>` —
  `include/modb/object/database.hpp:751`
- TODO `Snapshot` — `database.hpp:114-137` (move-only RAII, holds
  `DatabaseId` + epoch)
- TODO `Database::get<T>(id, const Snapshot&)` → `Result<T>` —
  `database.hpp:518-544` (value, not `Handle`)
- TODO `Database::scan<T>(const Snapshot&, visitor)` — `database.hpp:550-576`
- TODO `Database::collect_garbage()` → `Result<std::size_t>` —
  `database.hpp:956`
- TODO `Database::open_snapshot_count()` — leak diagnostic, `database.hpp:946-949`

## Semantics & invariants

- TODO: only one retained "previous" version per object — a second write
  while an older snapshot still needs it fails with `snapshot_conflict`
  *before* touching any bytes.
- TODO: `update`/`remove` never overwrite in place.
- TODO: `collect_garbage()` fails with `transaction_active` during an open
  write transaction; reclaims only what no open snapshot could still see.

## Common pitfalls

- TODO: leaking long-lived `Snapshot`s in a long-running process (blocks GC).
- TODO: expecting `get(id, snapshot)` to return a reusable `Handle`.

## Related documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 7
- [GARANTIAS_TRANSACIONAIS.md](../GARANTIAS_TRANSACIONAIS.md) §8-9
- TODO: confirm whether an ADR file documents epochs/IDMP v2 under
  [decisions/](../decisions/) and link it here

## Related source

- `include/modb/object/database.hpp` (`Snapshot`, `get`/`scan` overloads)
- `include/modb/object/identity_map.hpp`
- `examples/server/by_phase/phase_06/snapshot_read.cpp`
- `tests/snapshot_test.cpp`, `tests/mvcc_recovery_test.cpp`
