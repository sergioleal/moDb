# Relationships and Collections

> **Status:** 🚧 skeleton — outline only, not yet written. In the meantime see
> [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 6.

## Overview

- TODO: the three relationship kinds — association (`Ref`), composition
  (`OwnedRef`), embedding (`Embedded`) — and why `Ref`/`OwnedRef` share a wire
  shape.
- TODO: persistent collections (`PersistentVector`/`Set`/`Map`) as thin views
  over a `BlobStore`, not native member types.

## Core API surface

- TODO `Ref<T>` / `OwnedRef<T>` / `Embedded<T>` — `include/modb/object/ref.hpp:8-32`
- TODO `is_ref_v` / `is_owned_ref_v` / `is_embedded_v` — `ref.hpp:34-48`
  (compile-time recognition used by `BindingBuilder::field`/`attribute_type_of`)
- TODO `Database::remove(tx, id)` cascade semantics —
  `include/modb/object/database.hpp:958-1064` (depth-first `OwnedRef`
  cascade, cycle detection, plain `Ref` never followed)
- TODO `PersistentVector<T>` / `PersistentSet<T>` / `PersistentMap<K,V>` —
  `include/modb/object/collection.hpp` (`create`, `size`, `at`/`get`,
  `push_back`/`insert`/`put`, `remove`, `for_each`)
- TODO `Database::blobs()` → `BlobStore&`, `BlobId`

## Semantics & invariants

- TODO: dangling `Ref` after the target is removed → `record_not_found` on
  resolution, not corruption.
- TODO: `OwnedRef` ownership cycles are rejected (`invalid_argument`), not
  infinite-looped.
- TODO: `Embedded<T>` has no independent identity — never fetchable via
  `Database::get`.
- TODO: every collection mutation rewrites the entire underlying blob (MVP
  limitation, not a bug) — cost model for large collections.

## Common pitfalls

- TODO: forgetting to persist the `BlobId` on the parent object after
  `PersistentVector::create(...)`.
- TODO: mutating a collection without an active `Transaction&` from the same
  database as the `BlobStore`.

## Related documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 6
- [ADR-008](../decisions/ADR-008-integridade-de-referencias.md) — cascade and
  reference-integrity semantics

## Related source

- `include/modb/object/ref.hpp`, `collection.hpp`, `blob_store.hpp`
- `examples/server/by_phase/phase_04/relationships.cpp`
- `tests/collection_test.cpp`, `tests/relationship_test.cpp`
