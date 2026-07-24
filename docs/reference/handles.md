# Handles and Typed Access

> **Status:** 🚧 skeleton — outline only, not yet written. In the meantime see
> [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 5.

## Overview

- TODO: `Handle<T>` is pure identity (`DatabaseId` + `ObjectId`), not a cached
  object — every access round-trips through the database.
- TODO: relationship to schema evolution (lazy migration on read, rewrite on
  write).

## Core API surface

- TODO `Handle<T>` — `include/modb/object/handle.hpp:28-50` (private
  constructor, `friend class Database`)
- TODO `Handle<T>::get<Member>()` / `Handle<T>::set<Member>(tx, value)` —
  implementation in `include/modb/object/database.hpp:1642-1673`
- TODO `Database::get<T>(ObjectId)` → `Result<Handle<T>>` —
  `database.hpp:458-489`
- TODO `Database::materialize(const Handle<T>&)` — full decode to `T`
- TODO `Database::register_migration(...)` — custom (non-default-value)
  schema conversion — `database.hpp:741-742`

## Semantics & invariants

- TODO: `get<&T::member>()`/`set<&T::member>(...)` fully materialize/rewrite
  the whole object — not a columnar/partial operation.
- TODO: `Handle<T>` can only be constructed by `Database` (via `create`/`get`).
- TODO: lazy schema migration — old rows aren't rewritten until something
  calls `update()` on them.

## Common pitfalls

- TODO: assuming `set<&Member>` is O(1) regardless of `T`'s size.
- TODO: using a `Handle` obtained before a schema change without re-reading
  after `bind()` with the new version.

## Related documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 5
- [reference/object-model.md](object-model.md) — schema evolution and
  `BindingBuilder` default values

## Related source

- `include/modb/object/handle.hpp`, `database.hpp`
- `examples/server/by_phase/phase_03/handle_update.cpp`
- `tests/schema_evolution_test.cpp`
