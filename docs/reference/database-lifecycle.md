# Database Lifecycle

> Part of the `docs/reference/` set — see [docs/README.md](../README.md) for
> the full index and [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md) for a
> narrative, tutorial-style introduction. This document is a precise
> reference: it assumes you already know C++ well and want exact behavior,
> not a learning path.

## 1. Overview

A `Database` owns one on-disk instance (`<path>` plus its write-ahead log,
`<path>.wal`) and is the facade for everything else in this reference set:
binding types (§[object-model.md](object-model.md)), beginning transactions,
creating/reading objects through `Handle<T>`, taking snapshots, running
queries, and (indirectly) serving a network connection. It is move-only,
never copyable, and its lifetime is managed through a small process-wide
registry rather than raw pointers.

The one idea worth internalizing before anything else: **`Handle<T>`,
`Transaction`, and `Snapshot` never store a `Database*`.** They store a small
`DatabaseId` and re-resolve the live `Database` through `DatabaseRegistry`
on every single call. This is why a `Database` must be wrapped in a
`std::shared_ptr` and `attach()`-ed before you do anything with it — the
lifetime story only works if the registry, not your local variable, is the
thing keeping it alive.

## 2. Concepts

### 2.1 `create` vs `open`

```cpp
auto created = modb::object::Database::create(path);  // fails if `path` exists
auto opened  = modb::object::Database::open(path);     // fails if `path` doesn't exist
```

Both come in a plain overload and one that also takes a `DatabaseOptions`
(§2.3). Both accept an optional `cache_capacity` (page cache size), defaulting
to `storage::page_cache_capacity`.

`create` writes a fresh superblock and an empty catalog (the three reserved
meta-type ids, §[object-model.md §2.2](object-model.md#22-catalog-as-objects-bootstrapped-by-reserved-ids)).
`open` runs WAL recovery against whatever is already on disk before the
`Database` becomes usable — the caller never has to think about recovery
explicitly; it's folded into `open()` itself.

### 2.2 The `shared_ptr` + `DatabaseRegistry` pattern

```cpp
auto created = modb::object::Database::create(path);
auto database = std::make_shared<modb::object::Database>(std::move(*created));
auto id = modb::object::DatabaseRegistry::instance().attach(database);
// ... use database ...
modb::object::DatabaseRegistry::instance().detach(*id);
```
(`examples/server/by_phase/phase_02/persist_reopen.cpp`)

Every `Handle<T>`/`Transaction`/`Snapshot` you obtain from `database` stores
only `*id` internally. When you call, say, `handle.get<&T::member>()`, the
implementation does `DatabaseRegistry::instance().find(database_)` first,
and only proceeds if that lookup succeeds
(`include/modb/object/database.hpp:1642-1673`, confirmed in
`src/object/database.cpp:710-729,810-822`). Two direct consequences:

- If you never `attach()`, every `Handle` you create is unusable from the
  start.
- If you `detach()` while `Handle`s obtained from that database are still in
  use, they start failing (`type_not_found`/`invalid_argument`, "handle is
  not attached to a DatabaseRegistry") — but the `Handle` object itself
  doesn't crash; it's just two integers, so it fails gracefully at the next
  call.
- The `shared_ptr` inside the registry is what actually keeps the `Database`
  alive. Your local `database` variable going out of scope does nothing by
  itself as long as the registry still holds a reference.

### 2.3 `DatabaseOptions`

`include/modb/object/primary_storage.hpp:24-28`

```cpp
struct DatabaseOptions {
    PrimaryStorage primary_storage{PrimaryStorage::full};
    CommitAckPolicy commit_ack{CommitAckPolicy::local_wal};
    std::chrono::milliseconds commit_ack_timeout{std::chrono::seconds{5}};
    WalIoMode wal_io{WalIoMode::sync};
};
```

None of these have an implicit "smart" default that changes based on
context — every field defaults to the plain, single-instance, synchronous
behavior. Opting into anything else (`wal_only`, `await_one_replica`,
`async`) is always explicit, at `create`/`open` time:

- `PrimaryStorage{full, wal_only}` — `full` (default) keeps data pages
  locally; `wal_only` keeps *no* data pages at all, only the WAL and a small
  control block (`MCTL`, `include/modb/object/instance_control.hpp`) — see
  [OPERACAO_REPLICACAO.md](../OPERACAO_REPLICACAO.md).
- `CommitAckPolicy{local_wal, await_one_replica}` — when a `wal_only`
  primary considers a commit acknowledged to the caller. `local_wal`
  (default) doesn't wait for any replica.
- `WalIoMode{sync, async}` — the WAL's I/O backend
  (`NativeFile` vs `storage::AsyncFile`). See
  [OPERACAO_IO_ASSINCRONO.md](../OPERACAO_IO_ASSINCRONO.md) — as of this
  writing, `async` has shown no consistent benefit; `sync` is the
  recommended default in production.

### 2.4 Instance files: `full` vs `wal_only`

With `primary_storage=full` (the only mode that existed before Phase 15),
`path` is a real `PageFile` — the actual data. With `primary_storage=wal_only`,
`path` is instead an `MCTL` control file (identity + LSNs only); opening it
with the default options (`primary_storage=full`) deliberately fails, and
opening a `full` file with `primary_storage=wal_only` also fails — the mode
is a property of the file itself, not just a runtime toggle, and
`Database::open` cross-checks the two (`is_instance_control_file`,
`include/modb/object/instance_control.hpp`).

## 3. API Reference

### 3.1 `Database::create` / `Database::open`

`include/modb/object/database.hpp:378-390`

```cpp
[[nodiscard]] static Result<Database> create(
    const std::filesystem::path& path,
    std::size_t cache_capacity = storage::page_cache_capacity);
[[nodiscard]] static Result<Database> create(
    const std::filesystem::path& path, const DatabaseOptions& options,
    std::size_t cache_capacity = storage::page_cache_capacity);

[[nodiscard]] static Result<Database> open(
    const std::filesystem::path& path,
    std::size_t cache_capacity = storage::page_cache_capacity);
[[nodiscard]] static Result<Database> open(
    const std::filesystem::path& path, const DatabaseOptions& options,
    std::size_t cache_capacity = storage::page_cache_capacity);
```

`Database` itself:

```cpp
class Database {
public:
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) noexcept;
    Database& operator=(Database&&) = delete;
    ~Database();
    // ...
};
```
(`database.hpp:373-396`) — move-constructible, **not** move-assignable, never
copyable. The `PageFile` backing it is held by `unique_ptr` specifically so
its address stays stable across a `Database` move (the `ObjectStore` inside
holds a raw `PageFile*`).

### 3.2 `DatabaseRegistry`

`include/modb/object/database.hpp:345-356`

```cpp
class DatabaseRegistry {
public:
    [[nodiscard]] Result<DatabaseId> attach(std::shared_ptr<Database> database);
    [[nodiscard]] Result<std::shared_ptr<Database>> find(DatabaseId id) const;
    void detach(DatabaseId id);
    [[nodiscard]] static DatabaseRegistry& instance();
};
```

A process-wide singleton (`instance()`), backed by a mutex-guarded
`std::unordered_map<std::uint32_t, std::shared_ptr<Database>>`. `attach`
hands back a small runtime-only `DatabaseId` (never persisted — see
`include/modb/object/ids.hpp:33-38`); `find` is what every `Handle`/
`Transaction`/`Snapshot` calls internally.

### 3.3 Read-only replica controls

- `Database::is_read_only_replica() const noexcept` — `database.hpp:900`
- `Database::set_read_only_replica(bool)` → `Result<void>` — `database.hpp:901`
  (rejects enabling it on a `wal_only` primary — read-only is a *follower*
  concept, and a `wal_only` instance has no data to read locally at all; see
  `has_durable_data_files()` below)
- `Database::has_durable_data_files() const noexcept` — `database.hpp:897`
  (`true` only when `primary_storage == PrimaryStorage::full`)
- `Database::set_commit_ack_policy(CommitAckPolicy, std::chrono::milliseconds)`
  — `database.hpp:911` — override the ack policy after construction (mostly
  useful in tests; production code normally sets it once, via
  `DatabaseOptions`, at `create`/`open` time)

## 4. Semantics & Invariants

- `create` fails if the path exists; `open` fails if it doesn't — there is
  no "create or open" convenience, by design (accidentally overwriting or
  accidentally reusing a file are both mistakes worth surfacing explicitly).
- `Handle<T>`/`Transaction`/`Snapshot` resolve their owning `Database`
  through `DatabaseRegistry::find()` on **every** call — there is no
  caching of the pointer anywhere in these types.
- `DatabaseOptions` has no implicit upgrade path: passing the default
  `DatabaseOptions{}` always means `full` + `local_wal` + `sync`, regardless
  of what mode the file was last opened with.
- Opening a `wal_only` control file with default (`full`) options fails, and
  vice versa — the storage mode is a property of the file, checked at open
  time.
- `Database` is move-only; attempting to copy it is a compile error, and
  move-*assignment* is deleted too (only move-*construction* is allowed).

## 5. Common Pitfalls

- **Forgetting `detach()`.** The registry's `shared_ptr` keeps the `Database`
  (and its open file handles) alive for as long as it stays attached,
  independent of your local variable's scope — this is a real, easy-to-miss
  resource leak in long-running processes that open/close many databases.
- **Using a `Handle` after its `Database` was detached.** It won't crash —
  it'll return an error on the next call. Don't rely on this as a lifetime
  signal; detach only when you're truly done.
- **Assuming `DatabaseOptions` carries over between `create` and a later
  `open`.** It doesn't — you must pass the same (or an intentionally
  different) `DatabaseOptions` every time you open the file.
- **Trying to `create` a database at a path check with `std::filesystem::exists`
  first and racing another writer.** `create`'s own existence check is
  already the authority — don't build your own TOCTOU-prone pre-check on
  top of it.

## 6. Worked Example: Full Lifecycle Across Two "Sessions"

Condensed from `examples/server/by_phase/phase_02/persist_reopen.cpp`:

```cpp
// --- First process lifetime ---
{
    auto created = modb::object::Database::create(path);
    auto database = std::make_shared<modb::object::Database>(std::move(*created));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    database->bind(customer_binding());

    auto tx = database->begin();
    auto customer = database->create(*tx, Customer{"Ana", 42});
    tx->commit();
    const auto customer_id = customer->id();

    modb::object::DatabaseRegistry::instance().detach(*attached);
    // `database` (the shared_ptr) may now go out of scope; nothing else
    // holds a reference, so the file is closed here.
}

// --- Second process lifetime: reopen and read by ObjectId ---
{
    auto opened = modb::object::Database::open(path); // recovery already ran
    auto database = std::make_shared<modb::object::Database>(std::move(*opened));
    auto attached = modb::object::DatabaseRegistry::instance().attach(database);
    database->bind(customer_binding()); // must re-bind: bindings don't persist

    auto value = database->materialize(*database->get<Customer>(customer_id));
    // value->name == "Ana", value->score == 42

    modb::object::DatabaseRegistry::instance().detach(*attached);
}
```

## 7. Related Documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapters 1, 3
- [reference/object-model.md](object-model.md) — binding, catalog
- [OPERACAO_REPLICACAO.md](../OPERACAO_REPLICACAO.md) — `wal_only` primaries
  and read-only replicas in the replication context
- [OPERACAO_IO_ASSINCRONO.md](../OPERACAO_IO_ASSINCRONO.md) — `WalIoMode`
- [ADR-017](../decisions/ADR-017-primary-wal-only-sem-arquivos-de-dados.md)

## 8. Related Source

- `include/modb/object/database.hpp` (`Database`, `DatabaseRegistry`)
- `include/modb/object/primary_storage.hpp` (`DatabaseOptions`,
  `PrimaryStorage`, `CommitAckPolicy`, `WalIoMode`)
- `include/modb/object/instance_control.hpp` (`MCTL` control file, `wal_only`)
- `examples/server/by_phase/phase_02/persist_reopen.cpp`
- `tests/consumer/main.cpp` — smallest possible external consumer
- `tests/database_identity_test.cpp`, `tests/primary_storage_config_test.cpp`
