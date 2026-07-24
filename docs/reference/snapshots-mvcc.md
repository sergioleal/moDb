# Snapshots and MVCC

> Part of the `docs/reference/` set — see [docs/README.md](../README.md) for
> the full index and [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md) for a
> narrative, tutorial-style introduction. This document is a precise
> reference: it assumes you already know C++ well and want exact behavior,
> not a learning path.

## 1. Overview

A `Snapshot` gives you a consistent, point-in-time read view of the database
that stays stable even while other transactions keep committing. It is not a
copy of the data and not a lock — it's a single number (an *epoch*) that the
engine uses to decide, per object, whether to hand you the current version
or the previous one. This is classic single-version-back MVCC: exactly one
retained prior version per object, no more.

## 2. Concepts

### 2.1 A snapshot fixes an epoch, not a copy

```cpp
// include/modb/object/database.hpp:748-761
[[nodiscard]] Result<Snapshot> snapshot() {
    if (auto usable = check_usable(); !usable) return std::unexpected(usable.error());
    if (database_id_.value == 0) return std::unexpected(/* not attached */);
    const auto current_epoch = store_.epoch();
    register_snapshot_epoch(current_epoch);
    return Snapshot{database_id_, current_epoch};
}
```

Every commit advances a single monotonic epoch counter. `Database::snapshot()`
just captures the current value of that counter and registers it in an
in-memory `std::multiset<epoch>` so the garbage collector knows not to
reclaim anything that epoch might still need (§2.3). `Snapshot` itself
(`database.hpp:114-137`) is RAII, move-only, and — like `Handle`/
`Transaction` — stores only a `DatabaseId` (not a raw pointer), so it
survives the owning `Database` being moved. Its destructor unregisters the
epoch.

### 2.2 Reads through a snapshot: `current` vs `previous`

```cpp
// include/modb/object/database.hpp:518-544
template <typename T>
[[nodiscard]] Result<T> get(ObjectId id, const Snapshot& snapshot) {
    // ...
    auto object = store_.get_at(id, snapshot.epoch());
    // ...
    return materialize_decoded<T>(*bound, *object);
}
```

`store_.get_at(id, epoch)` picks, per the identity map: `current` if
`current_epoch ≤ snapshot.epoch`, otherwise `previous`, otherwise the object
didn't exist yet at that epoch. Creation/update/removal that happen *after*
the snapshot's epoch are simply invisible to it. `Database::scan<T>(snapshot, visitor)`
(`database.hpp:550-576`) is the same idea applied to a full type scan instead
of a single id.

**This is deliberately not a `Handle`.** `get(id, snapshot)` returns
`Result<T>` — a materialized value, right now, for that epoch. There is no
snapshot-scoped handle you can hold onto and re-read later expecting it to
track anything; a snapshot read is a value, not an identity.

### 2.3 One retained previous version, and `snapshot_conflict`

From [GARANTIAS_TRANSACIONAIS.md §8](../GARANTIAS_TRANSACIONAIS.md):

> O IdentityMap v2 guarda apenas `current` + `previous`. Uma segunda
> alteração de um objeto **enquanto a `previous` ainda é visível a um
> snapshot aberto mais antigo** falha com `snapshot_conflict`, antes de
> qualquer escrita — não há sobrescrita parcial.

In plain terms: the identity map has room for exactly two versions of an
object, `current` and `previous`. If object `X` is updated while some open
`Snapshot` still needs to see `X`'s `previous` version (because that
snapshot's epoch predates `current`), a *third* update to `X` would have
nowhere to put the version that snapshot still needs — so the engine refuses
the write up front with `snapshot_conflict`, before touching any bytes. If
no snapshot older than the current epoch is open, the update proceeds
normally and reuses the `previous` slot. The caller is expected to retry
(typically: begin a new transaction and try the update again) rather than
treat this as a hard failure.

`update()` never overwrites in place — it always inserts a fresh physical
record, so a retained `previous` stays byte-for-byte intact.
`remove()` doesn't physically delete either — it marks a logical tombstone
on the identity, leaving the old physical record in place until GC decides
it's safe to reclaim (§2.4).

### 2.4 Garbage collection is explicit, transactional, and epoch-aware

From [GARANTIAS_TRANSACIONAIS.md §9](../GARANTIAS_TRANSACIONAIS.md):

- `Database::collect_garbage()` runs in its own transaction — page
  reclamation goes through the WAL like any other write, so a crash mid-GC
  is atomic (all-or-nothing) and idempotent on reopen.
- It reconciles every physical heap record against the identity map: a live
  `current` is never touched; a `previous` is only reclaimed once the oldest
  *open* snapshot's epoch is `>=` that entry's `current` epoch — i.e. once
  **no** open snapshot could possibly still need it. While such a snapshot
  is open, GC returns `0` for that entry (nothing reclaimed, not an error).
- A physical record that's neither the referenced `current` nor `previous`
  (an orphan — e.g. a `previous` displaced by a second edit, or a leftover
  from a previous run) is always reclaimed unconditionally.
- Reclaiming a referenced `previous` calls `IdentityMap::clear_previous`,
  zeroing that slot while leaving `current` untouched. A tombstone whose
  `previous` was reclaimed becomes an empty-but-still-allocated entry — its
  `ObjectId` is **never** reused (ADR-001).
- Fails with `transaction_active` if a write transaction is currently open
  (single-writer applies to GC too).

## 3. API Reference

### 3.1 `Database::snapshot()` → `Result<Snapshot>`

`database.hpp:751` — see §2.1.

### 3.2 `Snapshot`

`database.hpp:114-137`

```cpp
class Snapshot {
public:
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Snapshot(Snapshot&&) noexcept;             // move-only
    Snapshot& operator=(Snapshot&&) = delete;
    ~Snapshot();

    [[nodiscard]] DatabaseId database() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
};
```

### 3.3 `Database::get<T>(ObjectId, const Snapshot&)` → `Result<T>`

`database.hpp:518-544` — fails with `invalid_argument` if the snapshot
belongs to a different `Database`; `type_not_found` if `T` isn't bound;
`type_mismatch` if the stored object is a different bound type.

### 3.4 `Database::scan<T>(const Snapshot&, visitor)` → `Result<void>`

`database.hpp:550-576` — visits every object of type `T` visible at the
snapshot's epoch, in physical order, skipping objects of other types. Uses
the same cached `ProjectionPlan` as `get`/`materialize`.

### 3.5 `Database::collect_garbage()` → `Result<std::size_t>`

`database.hpp:956` — see §2.4. Returns the count of physical records
actually reclaimed.

### 3.6 `Database::open_snapshot_count()` → `std::size_t`

`database.hpp:946` — diagnostic: how many `Snapshot`s are currently
registered against this database. Useful for catching snapshot leaks in a
long-running process (every open snapshot blocks GC from reclaiming
whatever it can still see).

## 4. Semantics & Invariants

- A snapshot's epoch is fixed at construction; nothing about it changes
  afterward, and its destructor always unregisters it.
- `get(id, snapshot)`/`scan(snapshot, ...)` return `current` if
  `current_epoch ≤ snapshot.epoch`, else `previous`, else "didn't exist yet".
- Exactly one `previous` version is retained per object — never more.
- A write that would need a *second* retained previous version (because an
  older snapshot still needs the current `previous`) fails atomically with
  `snapshot_conflict`, before any write happens.
- `update` always inserts a new physical record; `remove` only tombstones
  the identity — physical reclamation is GC's job, not the write path's.
- `collect_garbage()` never reclaims a version some open snapshot could
  still see, and never touches the live `current`.
- Snapshots do not survive the process — GC always cleans up leftover
  `previous` orphans from earlier runs on its next pass regardless.
- `ObjectId`s are never reused, even for a tombstoned, fully-GC'd entry
  (ADR-001).

## 5. Common Pitfalls

- **Leaking long-lived `Snapshot`s in a long-running process.** Every one
  still open blocks GC from reclaiming whatever it can see — check
  `open_snapshot_count()` if memory/disk seems to grow unexpectedly.
- **Expecting `get(id, snapshot)` to return a reusable `Handle`.** It
  returns a plain value for that instant — there is no "set through a
  snapshot read."
- **Treating `snapshot_conflict` as a hard error instead of a retry
  signal.** It's the engine telling you a concurrent old reader is in the
  way right now — the normal response is to retry the write in a new
  transaction.
- **Calling `collect_garbage()` while a write transaction is open.** Fails
  with `transaction_active` — run it between transactions.

## 6. Worked Example

Condensed from `examples/server/by_phase/phase_06/snapshot_read.cpp`:

```cpp
auto tx1 = database->begin();
auto account = database->create(*tx1, Account{"Alice", 100});
tx1->commit();

auto snap = database->snapshot(); // fixes the current epoch (balance == 100)

auto tx2 = database->begin();
database->get<Account>(account->id())->set<&Account::balance>(*tx2, 200);
tx2->commit();

auto stable  = database->get<Account>(account->id(), *snap);   // still 100
auto current = database->materialize(*database->get<Account>(account->id())); // now 200
```

Reclaiming space once the snapshot above is no longer needed:

```cpp
{
    auto snap = database->snapshot();
    // ... work that needs the old view ...
} // destructor unregisters the epoch here

auto reclaimed = database->collect_garbage();
// reclaimed->value may now include the Account's old `previous` version,
// since no open snapshot needs it anymore.
```

## 7. Related Documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 7
- [GARANTIAS_TRANSACIONAIS.md](../GARANTIAS_TRANSACIONAIS.md) §8-9 — the full
  narrative this document summarizes into reference form
- [ADR-009](../decisions/ADR-009-epocas-e-idmp-v2.md) — epochs and IDMP v2

## 8. Related Source

- `include/modb/object/database.hpp` (`Snapshot`, `snapshot()`,
  `get`/`scan` overloads, `collect_garbage`, `open_snapshot_count`)
- `include/modb/object/identity_map.hpp`
- `examples/server/by_phase/phase_06/snapshot_read.cpp`
- `tests/snapshot_test.cpp`, `tests/mvcc_recovery_test.cpp`
