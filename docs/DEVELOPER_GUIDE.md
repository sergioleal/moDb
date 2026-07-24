# Ring0 Developer Guide

A self-contained, step-by-step guide for a C++ developer using Ring0 (the
`modb` engine) for the first time. It teaches the product as it stands today —
not the order it was built in. Every code sample here is adapted from real,
working code in this repository; file references let you jump straight to the
full source.

If you get lost, [Chapter 17](#17-where-to-go-next) points to every other
document in this repo and what it's for.

## Table of Contents

0. [What Is Ring0](#0-what-is-ring0)
1. [Build, Verify, Install](#1-build-verify-install)
2. [Your First Type: `BindingBuilder<T>`](#2-your-first-type-bindingbuildert)
3. [Create, Persist, Reopen](#3-create-persist-reopen)
4. [Transactions](#4-transactions)
5. [`Handle<T>` and Typed Updates](#5-handlet-and-typed-updates)
6. [Relationships and Collections](#6-relationships-and-collections)
7. [Snapshots (MVCC)](#7-snapshots-mvcc)
8. [Indexes and Streaming Queries](#8-indexes-and-streaming-queries)
9. [Server and Client](#9-server-and-client)
10. [Remote Domain Operations](#10-remote-domain-operations)
11. [Facades: Versioned Remote Surfaces](#11-facades-versioned-remote-surfaces)
12. [Graphs](#12-graphs)
13. [Optional: Asynchronous WAL I/O](#13-optional-asynchronous-wal-io)
14. [Background: Read Replicas and `wal_only`](#14-background-read-replicas-and-wal_only)
15. [What's NOT Guaranteed](#15-whats-not-guaranteed)
16. [CLI Quick Reference](#16-cli-quick-reference)
17. [Where to Go Next](#17-where-to-go-next)

---

## 0. What Is Ring0

Ring0 is an **embedded, file-based, object-oriented database** written in
C++26. You bind ordinary C++ structs to a persistent catalog, get back stable
identities (`ObjectId`) for the objects you create, and the engine takes care
of durability (write-ahead log + crash recovery), consistent reads (MVCC
snapshots), streaming queries with optional indexes, and — if you want it —
a binary network protocol so a separate process can connect to your database
and stream queries or call in-process domain operations remotely.

It is **not** a SQL database, an ORM over an existing SQL engine, or a
document store. There is no query language string to parse: queries are a
typed, fluent C++ API. There is no reflection: every persisted field is
declared explicitly, once, with a stable numeric id.

**Prerequisites:**

- CMake ≥ 3.30, Ninja, a C++26 compiler (GCC 15+/Clang recent). If your local
  toolchain only supports C++23, the build has an explicit fallback (see
  Chapter 1) — GCC 13 works this way.
- Familiarity with modern C++: RAII, smart pointers, templates,
  `std::filesystem`, `std::expected`-style error handling.

---

## 1. Build, Verify, Install

Clone the repository, then:

```powershell
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

If your compiler doesn't support C++26 yet, opt into the local fallback:

```powershell
cmake --preset local-gcc13
cmake --build --preset local-gcc13
ctest --preset local-gcc13
```

### Sanity check: version and protocol negotiation

The smallest possible program that links against Ring0 checks the project
version and negotiates a protocol version — no database file involved yet
(`examples/server/by_phase/phase_00/version_compatibility.cpp`):

```cpp
#include "modb/compatibility.hpp"
#include "modb/version.hpp"

int main() {
    const modb::CompatibilityVersion client{1, 0};
    const modb::CompatibilityVersion server{1, 0};

    auto negotiated = modb::negotiate_protocol_version(client, server);
    if (!negotiated) {
        std::cerr << negotiated.error().message << '\n';
        return 1;
    }
    std::cout << modb::project_name() << ' ' << modb::project_version() << '\n';
    std::cout << "protocol " << negotiated->major << '.' << negotiated->minor << '\n';
}
```

```powershell
cmake --build --preset debug --target ring0_server_phase_00_version_compatibility
.\build\debug\ring0_server_phase_00_version_compatibility.exe
```

### Installing the library for your own project

Ring0 installs a real CMake package. From a build directory:

```powershell
cmake --install build/debug --prefix <prefix>
```

Two targets are exported: `modb::modb` (the engine, protocol, and server) and
`modb::app_client` (a small helper for applications that connect to a Ring0
server, see Chapter 9). Only headers listed in
[docs/API_PUBLICA.md](API_PUBLICA.md) are a stable contract — everything else
under `include/modb/` is for in-tree tools and tests only.

The smallest possible external consumer is `tests/consumer/main.cpp` — it
builds against nothing but installed headers plus `modb::modb`:

```cpp
#include <modb/object/database.hpp>
#include <modb/version.hpp>

int main() {
    auto created = modb::object::Database::create(path);
    if (!created) {
        std::cerr << "create failed: " << created.error().message << '\n';
        return 1;
    }
    auto opened = modb::object::Database::open(path);
    std::cout << modb::project_name() << ' ' << modb::project_version() << " consumer ok\n";
}
```

From here on, every chapter builds on this same `#include <modb/object/database.hpp>`
starting point.

---

## 2. Your First Type: `BindingBuilder<T>`

Ring0 doesn't use reflection or macros to map a C++ type to storage. You
declare the mapping explicitly with `BindingBuilder<T>`
(`include/modb/object/binding.hpp`). The type itself stays a plain struct
(`examples/server/by_phase/phase_01/bind_type.cpp`):

```cpp
struct Customer {
    std::string name;
    std::int64_t score{};
};

modb::object::BindingBuilder<Customer> customer_binding() {
    modb::object::BindingBuilder<Customer> builder{"Customer"};
    builder.field<1>("name", &Customer::name)
           .field<2>("score", &Customer::score);
    return builder;
}
```

`field<Id>(name, &T::member)` — `Id` is a compile-time `std::uint16_t`, not a
label. Bind it onto a database with `Database::bind`:

```cpp
if (auto bound = database->bind(customer_binding()); !bound) {
    std::cerr << bound.error().message << '\n';
    return 1;
}
```

**Why the numeric id matters — this is the single most important idea in this
chapter.** The id, not the field name, is what gets persisted on disk
(`FieldId`, `include/modb/object/ids.hpp`). It must be unique within the type
and is **never reused** across schema versions (see Chapter 5 on schema
evolution). Renaming `"score"` to `"points"` is free; changing `field<2>` to
`field<3>` for the same member is not — from the storage engine's point of
view you'd be creating a different field.

Other things worth knowing before you move on:

- `build()` (called internally by `bind`) rejects id `0`, duplicate ids,
  duplicate names, and an empty field list.
- Unsupported member types fail at **compile time** via a `static_assert` in
  `attribute_type_of<Member>()` — you cannot bind an unsupported type and only
  find out at runtime.
- `Database::bind` must be called **outside** any active transaction — it
  manages its own internal transaction to persist the catalog atomically.
- The same C++ type cannot be bound twice on one `Database` instance, and
  bindings are **not** restored automatically from the file — every process
  that opens the database must call `bind()` again with a builder describing
  the same fields.
- `Embedded<T>` members use `.embedded<Id>(...)`, not `.field<Id>(...)` — see
  Chapter 6.

---

## 3. Create, Persist, Reopen

```cpp
[[nodiscard]] static Result<Database> create(const std::filesystem::path& path,
                                              std::size_t cache_capacity = /* default */);
[[nodiscard]] static Result<Database> open(const std::filesystem::path& path,
                                            std::size_t cache_capacity = /* default */);
```

`create` fails if the file already exists; `open` fails if it doesn't.
`Database` is move-only. The full lifecycle —
create, write, close, reopen, read — from
`examples/server/by_phase/phase_02/persist_reopen.cpp`:

```cpp
// --- First process lifetime ---
auto created = modb::object::Database::create(path);
auto database = std::make_shared<modb::object::Database>(std::move(*created));
auto attached = modb::object::DatabaseRegistry::instance().attach(database);
database->bind(customer_binding());

auto tx = database->begin();
auto customer = database->create(*tx, Customer{"Ana", 42});
tx->commit();
const auto customer_id = customer->id();

modb::object::DatabaseRegistry::instance().detach(*attached);

// --- Second process lifetime: reopen and read by ObjectId ---
auto opened = modb::object::Database::open(path);
auto database2 = std::make_shared<modb::object::Database>(std::move(*opened));
auto attached2 = modb::object::DatabaseRegistry::instance().attach(database2);
database2->bind(customer_binding());

auto value = database2->materialize(*database2->get<Customer>(customer_id));
```

**The `shared_ptr` + `DatabaseRegistry` pattern is not a style choice — it's
required infrastructure.** `Handle<T>`, `Transaction`, and `Snapshot` never
hold a `Database*`. They only store a small `DatabaseId` and look up the live
`Database` through `DatabaseRegistry::instance().find(...)` on every single
call. This is why you must wrap your `Database` in a `shared_ptr` and
`attach()` it: if it's never attached (or is destroyed while still attached),
every `Handle`/`Transaction`/`Snapshot` obtained from it becomes unusable.

```cpp
[[nodiscard]] Result<DatabaseId> attach(std::shared_ptr<Database> database);
[[nodiscard]] Result<std::shared_ptr<Database>> find(DatabaseId id) const;
void detach(DatabaseId id);
static DatabaseRegistry& instance();
```

Forgetting to `detach()` leaks the registry slot — the `shared_ptr` keeps the
`Database` alive as long as it stays attached, regardless of whether your
local variable went out of scope.

---

## 4. Transactions

Every write goes through a `Transaction`. `bind()` is the one exception (it
uses its own internal transaction, see Chapter 2).

```cpp
[[nodiscard]] Result<Transaction> begin();
[[nodiscard]] Result<void> Transaction::commit();
[[nodiscard]] Result<void> Transaction::rollback();
template <typename T>
[[nodiscard]] Result<Handle<T>> create(Transaction& tx, const T& value);
```

`examples/server/by_phase/phase_05/transaction_recovery.cpp`:

```cpp
auto tx = database->begin();
auto account = database->create(*tx, Account{"Alice", 100});
if (!account || !tx->commit()) {
    std::cerr << "failed to commit Account\n";
    return 1;
}
```

**Rules that matter in practice:**

- **Single-writer.** `begin()` fails with `transaction_active` if a
  transaction is already open on that `Database`.
- **RAII rollback-on-scope-exit.** If a `Transaction` is destroyed without an
  explicit `commit()`, it rolls back automatically. Forgetting to call
  `commit()` is not a silent bug — it's the safety net: nothing you wrote
  survives.
- **Commit protocol is WAL-before-pages**: (1) write `begin` + page images to
  the WAL, (2) `sync` the WAL, (3) write the `commit` record + `sync` — *this
  is the actual durability point* — (4) apply pages to the data file + flush,
  (5) checkpoint. A crash before step 3 means the transaction never happened;
  a crash after step 3 means it will be redone automatically the next time you
  `open()` the database.
- **Once the commit record is durable, the transaction is consumed** even if
  something fails afterward while applying pages — `rollback()` is refused
  (`commit_recovery_required`) and you must reopen the `Database` to let
  recovery replay the WAL.
- Every write operation — `create`, `Handle::set`, collection mutations —
  takes a `Transaction&` and fails with `transaction_required` if none is
  active.

Want to see a real crash and recovery? `modb tx crash <file> before-commit`
from the CLI kills the process mid-commit on purpose so you can inspect what
survives (Chapter 16).

---

## 5. `Handle<T>` and Typed Updates

`Database::get<T>(id)` returns a `Handle<T>`, not the object itself:

```cpp
template <typename T>
[[nodiscard]] Result<Handle<T>> get(ObjectId id);

template <auto Member>
[[nodiscard]] Result<member_type_t<decltype(Member)>> Handle<T>::get() const;
template <auto Member, typename V>
[[nodiscard]] Result<void> Handle<T>::set(Transaction& transaction, V&& value) const;
```

`examples/server/by_phase/phase_03/handle_update.cpp`:

```cpp
auto tx = database->begin();
auto account = database->create(*tx, Account{"Alice", 100});
if (!account || !account->set<&Account::balance>(*tx, 125) || !tx->commit()) {
    std::cerr << "failed to update Account\n";
    return 1;
}
auto current = database->materialize(*database->get<Account>(account->id()));
```

**This is the single most important thing to understand about `Handle<T>`:**
it is **pure identity** — a `DatabaseId` and an `ObjectId`, nothing else. It
does not own or cache the object. Every `get<&T::member>()` or
`set<&T::member>(tx, value)` call **fully materializes the entire object**
internally, reads or mutates the one member you asked for in memory, and — for
`set` — **rewrites the whole object** back to storage. Updating one field out
of twenty costs roughly the same as updating all twenty. `Handle<T>` can only
be constructed by `Database` (via `create`/`get`) — you never build one
yourself.

**Schema evolution is lazy.** Add a new field with a default value:

```cpp
builder.field<3>("country", &EmployeeV2::country, "BR");
```

`bind()` registers the new type version; **existing rows on disk are not
rewritten**. When an old record is read through the new binding, the missing
field is filled in from the declared default. The row only gets physically
rewritten the next time something calls `update()` on it (e.g. via
`Handle::set`). For conversions that aren't a simple default value,
`Database::register_migration(...)` accepts a custom function from the old
decoded shape to the new one.

---

## 6. Relationships and Collections

Ring0 has three relationship kinds, all in `include/modb/object/ref.hpp`, plus
persistent collections in `include/modb/object/collection.hpp`.

```cpp
template <typename T> struct Ref      { ObjectId target{}; }; // association
template <typename T> struct OwnedRef { ObjectId target{}; }; // composition (cascade)
template <typename T> struct Embedded { T value{}; };         // no identity, inline
```

`examples/server/by_phase/phase_04/relationships.cpp`:

```cpp
struct Employee {
    std::string name;
    modb::object::Ref<Department> department{};
    modb::object::OwnedRef<Badge> badge{};
};
builder.field<1>("name", &Employee::name)
       .field<2>("department", &Employee::department)
       .field<3>("badge", &Employee::badge);

auto department = database->create(*tx, Department{"Engineering"});
auto badge = database->create(*tx, Badge{7});
auto employee = database->create(*tx, Employee{"Ana", {department->id()}, {badge->id()}});
```

**`Ref` and `OwnedRef` share the same wire shape** — an `ObjectId` — the only
difference is a flag that controls delete cascade:

- `Database::remove(tx, id)` follows every `OwnedRef` depth-first and deletes
  the owned targets too. A composition cycle is detected and rejected
  (`invalid_argument`) rather than infinite-looping.
- Plain `Ref` targets are **never** followed on delete. The target simply
  disappears; resolving the dangling reference later returns
  `record_not_found`. This is expected, detectable state, not corruption.

`Embedded<T>` requires its own nested `Binding` supplied at bind time and has
no independent identity — you can never `Database::get` an embedded value on
its own; it only exists as part of its parent's payload.

**Persistent collections** (`PersistentVector<T>`, `PersistentSet<T>`,
`PersistentMap<K, V>`) are not native member types. The parent object stores
only a `BlobId`; the collection object itself is a thin, stateless view over a
`BlobStore` plus that id:

```cpp
auto blobs = database->blobs();
auto projects = PersistentVector<Ref<Project>>::create(blobs, *transaction);
projects->push_back(*transaction, Ref<Project>{some_id});
employee.projects = projects->id(); // you persist the BlobId yourself

// later:
PersistentVector<Ref<Project>> reopened{blobs, employee.projects};
reopened.for_each([](const Ref<Project>& ref) { /* ... */ return Result<void>{}; });
```

Every mutator (`push_back`, `insert`, `put`, `remove`) requires an active
`Transaction&` on the same database as the `BlobStore`. **Known MVP
limitation:** every mutation rewrites the entire underlying blob — these
collections are fine for small sets of related objects, not an
append-optimized structure at scale.

---

## 7. Snapshots (MVCC)

```cpp
[[nodiscard]] Result<Snapshot> snapshot();
template <typename T>
[[nodiscard]] Result<T> get(ObjectId id, const Snapshot& snapshot);
template <typename T>
[[nodiscard]] Result<void> scan(const Snapshot& snapshot,
                                 const std::function<Result<void>(const T&)>& visitor);
[[nodiscard]] Result<std::size_t> collect_garbage();
```

`examples/server/by_phase/phase_06/snapshot_read.cpp`:

```cpp
auto tx1 = database->begin();
auto account = database->create(*tx1, Account{"Alice", 100});
tx1->commit();

auto snap = database->snapshot();   // fixes the current epoch

auto tx2 = database->begin();
database->get<Account>(account->id())->set<&Account::balance>(*tx2, 200);
tx2->commit();

auto stable  = database->get<Account>(account->id(), *snap);   // still 100
auto current = database->materialize(*database->get<Account>(account->id())); // now 200
```

**Key differences from `Handle<T>`:** a snapshotted read returns a plain
value (`Result<T>`), not a `Handle` — it's a point-in-time value, not a
reusable identity you can later `set()` through.

`Snapshot` fixes an *epoch*, not a data copy. Ring0's MVCC only keeps **one
retained "previous" version per object.** If a second write happens to an
object while an older snapshot can still see its `previous` version, that
write fails immediately with `snapshot_conflict` — before touching any bytes —
and the writer must retry. `update`/`remove` never overwrite in place, so a
retained `previous` always stays intact until it's safe to reclaim.

**Garbage collection is explicit, not automatic**: call
`Database::collect_garbage()` yourself (it fails with `transaction_active` if
a write transaction is open). It reclaims `previous` versions once no open
snapshot could still need them. If you never call it, retained versions
accumulate. `Database::open_snapshot_count()` is a diagnostic for finding
snapshot leaks in a long-running process.

---

## 8. Indexes and Streaming Queries

`Database::query<T>()` opens a snapshot at the current epoch and returns a
fluent, lazy builder. Nothing runs until `.stream()`:

```cpp
Query<T> Database::query();
Query&& Query::where(std::function<bool(const T&)>) &&;
Query&& Query::limit(std::size_t) &&;
Query&& Query::equals(FieldId, AttributeValue) &&;
Query&& Query::between(FieldId, AttributeValue, AttributeValue) &&;
query::Generator<Result<T>> Query::stream() &&;
query::QueryPlan Query::plan() const;
```

`examples/server/by_phase/phase_07/streaming_query.cpp`:

```cpp
for (auto& result : database->query<Item>()
                          .where([](const Item& item) { return item.value % 2 == 0; })
                          .limit(2)
                          .stream()) {
    if (!result) { std::cerr << result.error().message << '\n'; return 1; }
    std::cout << result->name << '\n';
}
```

`.stream()` returns a lazy `query::Generator` — a C++20 coroutine generator.
Storage is only touched as you iterate; abandoning the loop early (e.g. after
`break`) is safe and cleans up correctly. This is the reason `.stream()` is
rvalue-qualified: the whole builder chain is meant to be used once, fluently.

**Indexes** speed up `equals`/`between` from a full table scan to an index
scan:

```cpp
[[nodiscard]] Result<void> create_index<T>(FieldId field);
```

`create_index` builds a B+ tree, backfills existing rows, and registers it in
the catalog inside its own transaction — call it outside any open write
transaction, after the type is bound.

**The query planner is deterministic, not cost-based** (there's no cardinality
estimation in this engine): an `equals`/`between` uses an index if one exists
on that field, else a table scan; `order_by` + `limit` becomes a Top-K
selection; plain `order_by` or `distinct` fully materializes and sorts (this
defeats the "streaming, O(1) memory" benefit — a good thing to keep in mind
before assuming every query stays cheap); a plain `limit` on an otherwise
streaming plan is pushed all the way down to the source. `Query::plan()`
(or `--explain` from the CLI) shows you exactly which of these rules fired.

---

## 9. Server and Client

Ring0 can also be used over a small versioned binary protocol, so a separate
client process streams queries or calls remote operations against a server
process that owns the database file.

```cpp
static Result<Server> Server::listen(path, host = "127.0.0.1", port = 0); // port 0 = OS picks one
Result<void> Server::serve_one();      // one client session
Result<void> Server::serve_forever();  // loop until request_stop()

static Result<ServerConnection> ServerConnection::connect(ConnectionOptions);
static Result<ServerInfo> ServerConnection::handshake(ConnectionOptions);
```

`examples/server/by_phase/phase_08/connect_query.cpp`:

```cpp
auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
std::thread acceptor([&server] { (void)server->serve_one(); });

auto connection = modb::app::ServerConnection::connect({
    .host = "127.0.0.1",
    .port = server->port(),
    .database_name = std::string{server->database_name()},
});
auto rows = connection->collect(modb::net::QueryDescription{ .type = *item_type, .limit = 10 });
```

Note that a remote query uses `QueryDescription` with a *negotiated type id*,
not a C++ template — the client may be a different binary that doesn't share
your type definitions.

`ServerConnection::handshake` alone (no query sent) is how you inspect what
the server negotiated (`examples/server/by_phase/phase_10/handshake_capabilities.cpp`):

```cpp
auto info = modb::app::ServerConnection::handshake({...});
std::cout << "protocol " << info->protocol_major << '.' << info->protocol_minor
          << " max_streams=" << info->max_concurrent_streams << '\n';
```

**Protocol limits worth knowing** (`include/modb/net/protocol.hpp`):

- Max frame size 16 MiB; strings inside a frame capped at 64 KiB.
- Default max concurrent streams per connection: 4. Default max expansion
  ratio for decompression: 8× (a defensive cap against decompression bombs).
- Compression: `none` is always supported; `rle` is negotiated with a
  fallback to `none`.
- **Backpressure**: the server batches produced objects and force-flushes once
  a batch reaches `max_in_flight_objects = 8` (`include/modb/net/server.hpp`)
  — this is the concrete, fixed backpressure limit on the wire.
- **Cancellation is cooperative**: a `Cancel{query_id}` frame stops the
  server-side generator, but the server still replies with a `StreamEnd`
  carrying the partial count, and **the same connection stays usable** for a
  new query afterward.
- The server enforces single-writer at the `Database` level regardless of how
  many clients or streams are connected — networking doesn't change the
  transaction model from Chapter 4.

---

## 10. Remote Domain Operations

Sometimes you want business logic to run *in the server process*, next to the
data, and be callable by id instead of shipping a query. That's an
`Operation`.

```cpp
class Operation {
public:
    virtual std::string_view id() const noexcept = 0;
    virtual OperationMode mode() const noexcept { return OperationMode::read_write; }
    virtual Result<OperationResult> execute(ExecutionContext& context) = 0;
};

Result<void> OperationRegistry::register_factory(std::string id, OperationFactory, OperationMode = read_write);
Result<OperationResult> OperationRegistry::dispatch(std::string_view id, std::span<const std::byte> args, Database&);
```

`dispatch()` runs begin → `execute()` → commit, and rolls back on any error
*or thrown exception*. A full example, `examples/transfer_funds/`:

```cpp
class TransferFunds final : public ops::Operation {
public:
    static constexpr std::string_view k_id = "account.transfer";
    static constexpr ops::OperationMode k_mode = ops::OperationMode::read_write;

    Result<ops::OperationResult> execute(ops::ExecutionContext& context) override {
        // ...load source_account/dest_account via context...
        if (source_account->balance < amount_) {
            return std::unexpected(Error{ErrorCode::invalid_argument, "insufficient funds"});
        }
        source_account->balance -= amount_;
        dest_account->balance += amount_;
        access.update(*source_handle, *source_account);
        access.update(*dest_handle, *dest_account);
        return ops::OperationResult{};
    }
};
```

Modules are registered through a `ModuleManifest` (id, version, exported
method ids/modes, and a content `hash`) plus an explicit admission allowlist —
a production-style guard against loading unexpected code
(`examples/server/by_phase/phase_09/call_operation.cpp`):

```cpp
auto registry = std::make_shared<modb::ops::OperationRegistry>();
modb::ops::ModuleLoader loader;
const auto manifest = modb::examples::transfer_funds_manifest(baseline);
loader.admit_hash(manifest.hash);
loader.load(manifest, baseline, *registry, modb::examples::register_transfer_funds_module);
server->set_operation_registry(registry);
```

A remote client calls it by id, with hand-encoded arguments (no reflection):

```cpp
auto args = modb::examples::TransferFunds::encode_args(alice, bob, 40);
auto result = connection->call(modb::examples::TransferFunds::k_id, *args);
```

Validate business rules (like the balance check above) *before* mutating
anything — a failed `Result` or a thrown exception both roll back cleanly, but
only if you haven't already left the object graph half-updated in a way the
rollback wouldn't undo (it undoes the *transaction*, not manual in-memory
state you might have kept elsewhere).

---

## 11. Facades: Versioned Remote Surfaces

A facade groups and versions a set of operations into a typed surface a
client can call without knowing the server's concrete C++ types.

```cpp
template <typename TFacade>
class FacadeHandle {
public:
    static Result<FacadeHandle> open(const FacadeCatalog&, OperationRegistry&, Database&); // embedded
    static Result<FacadeHandle> open(FacadeDescriptor, FacadeInvoker);                     // remote
    template <typename Method, typename... Args>
    Result<OperationResult> invoke(Args&&... args);
};
```

`examples/server/by_phase/phase_11/open_facade.cpp`:

```cpp
auto handle = connection->open_facade<modb::examples::AccountsFacade>();
auto result = handle->invoke<modb::examples::TransferFunds>(alice, bob, 25);
```

The client only depends on `TFacade::k_id`/`k_version` and each
`Method::k_id`/`encode_args` — never on the server's concrete implementation.
`invoke<Method>` looks the method up in the negotiated `FacadeDescriptor`,
encodes the arguments, and dispatches through whatever `FacadeInvoker` was
supplied (a local closure over `OperationRegistry::dispatch`, or a network
call — the call site looks identical either way).

**Facade identity is `FacadeId + facade_version`, never a catalog array
index.** Calling with a stale/wrong version gets
`incompatible_facade_version`; an unknown facade id gets `facade_not_found`;
calling a method that isn't part of that facade gets
`facade_method_not_found`. Domain errors from inside the operation (e.g.
insufficient balance) propagate as a normal failed `Result`, same as Chapter
10 — a facade is a naming/versioning layer, not a second execution engine.

---

## 12. Graphs

`GraphView` gives you typed traversal over `Ref`/`OwnedRef` edges, anchored to
a `Snapshot` you keep alive yourself:

```cpp
Result<GraphView> open_graph_view(Database&, const Snapshot&);

template <class From, class To>
Result<std::vector<EdgeHandle<From, To>>>
outgoing_collection(const Handle<From>& source, FieldId field, BlobId From::* member) const;

template <class From, class To>
Result<std::vector<EdgeHandle<From, To>>>
incoming(const Handle<To>& target, FieldId field, Ref<To> From::* member) const;
```

**`incoming()` requires a B+ index on the pointer field** — there's no reverse
scan fallback; without one you get `invalid_edge`.

`EdgeHandle<From, To>` is a lightweight, non-persisted runtime view:

```cpp
Result<From> source(const Snapshot&) const;
Result<To>   target(const Snapshot&) const;  // record_not_found -> edge_target_not_found
Result<bool> dangling(const Snapshot&) const;
```

Traversal is a lazy generator, same shape as streaming queries
(`examples/server/by_phase/phase_12/graph_traversal.cpp`):

```cpp
auto adjacency = [&edges](ObjectId from) -> Result<std::vector<ObjectId>> { return edges[from.value]; };
for (auto& item : modb::graph::bfs(ObjectId{1}, adjacency)) {
    if (!item) { std::cerr << item.error().message << '\n'; return 1; }
    std::cout << "visit " << item->id.value << " depth=" << item->depth << '\n';
}
```

Algorithm helpers on the same `AdjacencyFn` shape:
`shortest_path` (unreachable → `record_not_found`),
`has_cycle`, `topological_sort` (cyclic input → `graph_cycle`), and
`connected_components` (needs an adjacency function that already returns both
directions). All of them respect `TraversalOptions{max_depth, max_vertices}`
and cooperative cancellation, returning `graph_limit_exceeded` if a limit is
hit.

---

## 13. Optional: Asynchronous WAL I/O

By default the WAL is written synchronously (`NativeFile`). There's an
opt-in, experimental async path:

```cpp
DatabaseOptions opts;
opts.wal_io = WalIoMode::async; // default: WalIoMode::sync
auto db = Database::create(path, opts);
```

Internally this swaps the WAL sink for one built on `storage::AsyncFile`
(IOCP on Windows, POSIX AIO on Linux), which batches a transaction's
after-images and drains everything in one barrier instead of one blocking
write per record. It does **not** change the WAL format or the
WAL-before-pages ordering from Chapter 4.

**Be honest with yourself about the payoff.** The measured benchmark
(`storage.async_io`, see [docs/OPERACAO_IO_ASSINCRONO.md](OPERACAO_IO_ASSINCRONO.md))
found **no consistent win** at the transaction sizes tested — anywhere from
31% slower to 6% faster, on both backends, sign flipping between runs.
**Keep `WalIoMode::sync` (the default) unless you've measured your own
workload and it actually helps.** Treat `async` as experimental.

---

## 14. Background: Read Replicas and `wal_only`

Two related, more advanced capabilities exist and are worth knowing about,
even if you don't need them on day one:

- **Read replicas**: a primary is the sole writer; a follower keeps its own
  copy of the data file and applies the primary's WAL, serving read-only
  queries. Bootstrap with `modb replicate bootstrap primary.modb
  follower.modb`, catch up incrementally with `modb replicate apply-wal`.
- **`PrimaryStorage::wal_only`**: a primary that keeps *no data-file pages at
  all* — only its WAL and a small control block — pushing all data pages onto
  its replicas. Since it has nothing to copy, followers are seeded with
  `modb replicate seed-wal` instead of a file-copy bootstrap.
  `CommitAckPolicy` controls whether a client's commit waits for at least one
  data replica to acknowledge it.

Full depth: [docs/OPERACAO_REPLICACAO.md](OPERACAO_REPLICACAO.md).

---

## 15. What's NOT Guaranteed

A few documented, intentional limitations of the current engine (see
[docs/GARANTIAS_TRANSACIONAIS.md](GARANTIAS_TRANSACIONAIS.md) for the full
list):

- **Page allocation is immediate, not transactional.** Pages allocated by a
  transaction that later rolls back stay allocated — orphaned, not corrupt —
  visible to `modb db check`. There's no free list reclaiming them yet.
- **Checkpointing removes the whole WAL**, there's no incremental
  checkpoint. If that removal itself fails, the WAL simply stays around for
  an idempotent redo on the next open — it's not silently lost.
- **MVCC keeps exactly one retained "previous" version.** A second concurrent
  writer against an object an old snapshot still needs gets
  `snapshot_conflict` immediately, before any bytes are touched, rather than
  queuing or blocking.
- **Garbage collection is `O(number of heap records)`** — a full
  reconciliation each time you call `collect_garbage()`, not an incremental
  sweep.
- **`BlobStore` used directly (not through a collection) has no transactional
  guard of its own** — it's a low-level primitive, same trust level as the
  CLI's `blob`/`record`/`heap` diagnostic commands.

---

## 16. CLI Quick Reference

Build once: `cmake --build build/debug` → `build/debug/modb` (`modb.exe` on
Windows). Full reference: [docs/USO_DA_CLI.md](USO_DA_CLI.md). The handful of
commands worth knowing as a learner:

```text
modb demo run --force          # runs every demo below, in order, for real
modb db create <file>
modb db check <file>
modb oo employee index <file> --schema 2
modb query <file> --schema 2 --salary 18000 --explain
modb tx crash <file> before-commit --force
modb tx get <file> <object-id>
modb serve <file> --port N --once
modb ping <host> <port> <database-name>
```

Two transcripts worth trying yourself:

```text
$ modb query phase3.modb --schema 2 --salary 18000 --limit 1 --explain
plan: access=index_scan nature=streaming first_result_cost=1 limit_pushed=true index_requested=true index_available=true
Employee: name=Bia salary=18000 country=PT
1 employee(s) streamed (via index) (nature=streaming); data pages read: 0
```

```text
$ modb tx crash conta.modb before-commit --force
staged Account{id=18, owner=Ana, balance=1000}
commit stopped BEFORE the commit record: only page images reached the WAL
recovery will discard this transaction entirely
...
$ modb tx get conta.modb 18
WAL before opening: present
WAL after opening (recovery already ran): present
Account 18: absent -- that transaction never became durable
```

A couple of conventions: domain errors print `Error: <message>` and exit code
`1`; a misused/unknown command exits `2`. There is no raw `object create` —
writes always go through the transactional `Database` API so they can't
bypass the WAL.

---

## 17. Where to Go Next

- **[docs/USO_DA_CLI.md](USO_DA_CLI.md)** — the complete CLI reference (every
  command, every flag).
- **`training/en/`** — one English lesson per major capability, each with a
  standalone runnable example under `examples/server/by_phase/`; a deeper,
  slower-paced companion to this guide.
- **[docs/API_PUBLICA.md](API_PUBLICA.md)** — the exact installed-header
  contract if you're consuming Ring0 as a library from another project.
- **[docs/GARANTIAS_TRANSACIONAIS.md](GARANTIAS_TRANSACIONAIS.md)** — the full
  transactional/MVCC guarantee document Chapters 4, 7, and 15 summarized.
- **[docs/FACADES.md](FACADES.md)**, **[docs/OPERACAO_REPLICACAO.md](OPERACAO_REPLICACAO.md)**,
  **[docs/OPERACAO_IO_ASSINCRONO.md](OPERACAO_IO_ASSINCRONO.md)** — operational
  depth for Chapters 11, 13, and 14.
- **[docs/RASTREADOR.md](RASTREADOR.md)** and **[docs/PLANO_ODB.md](PLANO_ODB.md)**
  — the project's construction history and roadmap, if you're curious how any
  of this was built or what's still planned.
- **`docs/decisions/`** — the ADRs behind specific design choices mentioned
  throughout this guide (schema evolution, ownership/cascade semantics,
  facades, replication, async I/O, and more).
