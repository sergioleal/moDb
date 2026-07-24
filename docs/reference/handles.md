# Handles and Typed Access

> Part of the `docs/reference/` set — see [docs/README.md](../README.md) for
> the full index and [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md) for a
> narrative, tutorial-style introduction. This document is a precise
> reference: it assumes you already know C++ well and want exact behavior,
> not a learning path.

## 1. Overview

`Handle<T>` is how you refer to a persisted object once it exists.
Everything about it follows from one design decision, stated directly in the
source: a handle is "apenas identidade... como manda arquitetura.md §6" —
*just identity*. It carries a `DatabaseId` and an `ObjectId` and nothing
else: no cached fields, no pointer to the object, no buffer. Every read or
write through a `Handle<T>` re-fetches (and, for writes, re-persists) the
whole object, on the spot.

This matters because it's easy to assume a "handle" behaves like a smart
pointer to a live, cached object (as in many ORMs). It doesn't. It behaves
closer to a strongly-typed foreign key that happens to have convenient
`get`/`set` methods attached.

## 2. Concepts

### 2.1 A handle is two integers, constructed only by `Database`

```cpp
template <typename T>
class Handle {
public:
    [[nodiscard]] DatabaseId database() const noexcept { return database_; }
    [[nodiscard]] ObjectId id() const noexcept { return id_; }

    template <auto Member>
    [[nodiscard]] Result<member_type_t<decltype(Member)>> get() const;

    template <auto Member, typename V>
    [[nodiscard]] Result<void> set(Transaction& transaction, V&& value) const;

private:
    friend class Database; // only Database can construct a Handle
    explicit Handle(DatabaseId database, ObjectId id) noexcept;

    DatabaseId database_;
    ObjectId id_;
};
```
(`include/modb/object/handle.hpp:28-50`)

The constructor is private with `friend class Database` — you cannot
fabricate a `Handle<T>` yourself from a raw `ObjectId` you found somewhere.
You only ever get one back from `Database::create<T>(tx, value)` or
`Database::get<T>(id)`. This is deliberate: constructing one implies the
`Database` already checked the id refers to an object of the expected type
(see §3.2).

`Handle<T>` is trivially copyable, has `operator==` (defaulted), and costs
essentially nothing to pass around — treat it like a lightweight value type,
not a resource.

### 2.2 `get<&T::member>()` and `set<&T::member>(tx, value)` are full round-trips

```cpp
template <typename T>
template <auto Member>
Result<member_type_t<decltype(Member)>> Handle<T>::get() const {
    auto database = DatabaseRegistry::instance().find(database_);
    if (!database) return std::unexpected(database.error());
    auto object = (*database)->materialize(*this);   // decodes the WHOLE object
    if (!object) return std::unexpected(object.error());
    return (*object).*Member;                        // ...then reads one member
}

template <typename T>
template <auto Member, typename V>
Result<void> Handle<T>::set(Transaction& transaction, V&& value) const {
    if (transaction.database() != database_) {
        return std::unexpected(Error{ErrorCode::invalid_argument,
                                     "transaction belongs to a different database"});
    }
    auto database = DatabaseRegistry::instance().find(database_);
    if (!database) return std::unexpected(database.error());
    auto object = (*database)->materialize(*this);   // decode...
    if (!object) return std::unexpected(object.error());
    (*object).*Member = std::forward<V>(value);       // ...mutate one member in memory...
    return (*database)->update(transaction, *this, *object); // ...then re-persist ALL of it
}
```
(`include/modb/object/database.hpp:1642-1673`)

There is no partial/columnar path anywhere in this call chain. `get<Member>`
decodes the entire stored payload (`materialize`, which runs the full
`ProjectionPlan`) just to hand you back one field. `set<Member>` does the
same decode, mutates one field in the resulting in-memory `T`, and then
calls `Database::update(tx, handle, value)` (`database.hpp:581`), which
re-serializes and rewrites the *entire* object. Updating 1 field out of 20
costs the same as updating all 20 — there is no way around this today for a
single-field mutation through `Handle`. If you need to change several
fields at once, do it in one `materialize()` → mutate all fields locally →
one `update()` (or one `set<Member>` call per field is fine too, it's just
paying the full round-trip cost each time; batching into a single explicit
`update()` call avoids paying it repeatedly).

### 2.3 Resolution goes through `DatabaseRegistry` every time

Both `get` and `set` start with
`DatabaseRegistry::instance().find(database_)` — there is no cached pointer
inside `Handle<T>` to skip this. See
[reference/database-lifecycle.md §2.2](database-lifecycle.md) for what
happens if the owning `Database` was `detach()`-ed: the lookup fails, and
`get`/`set` return that failure as a normal `Result` error — the `Handle`
itself is never in an invalid *memory* state, it's just semantically stale.

### 2.4 Lazy schema migration on read

If the handle's static type `T` corresponds to a newer catalog version than
the one the stored object was actually written with, `materialize()` applies
a `ProjectionPlan`: missing fields get filled from whatever default was
declared in the newer `BindingBuilder` (see
[reference/object-model.md §2.4](object-model.md#24-schema-evolution-and-lazy-migration)).
This happens transparently inside `get`/`materialize` — you don't do
anything differently. The stored bytes are **not** rewritten just because
you read them this way; only a subsequent `set`/`update()` physically
rewrites the record in its new shape.

For conversions a plain default value can't express,
`Database::register_migration(type_name, from_type_id, Migration fn)`
(`database.hpp:741`, where
`using Migration = std::function<Result<FieldValues>(const DecodedObject&)>;`)
lets you supply custom logic instead of (or in addition to) declared
defaults.

## 3. API Reference

### 3.1 `Database::get<T>(ObjectId)` → `Result<Handle<T>>`

`include/modb/object/database.hpp:458-489`

Checks, in order: the database is usable (§[database-lifecycle.md](database-lifecycle.md)),
durable data is available (fails with `data_files_disabled` on a `wal_only`
primary — see [OPERACAO_REPLICACAO.md](../OPERACAO_REPLICACAO.md)), the
database is attached, `T` is bound, and — via `peek_type` (a Phase 10C
optimization that avoids a full payload decode just to check the type,
`database.hpp:474-476`) — that the stored object's type matches `T`'s bound
type name (`type_mismatch` otherwise). Only after all of that does it
construct and return the `Handle<T>`.

### 3.2 `Database::create<T>(Transaction&, const T&)` → `Result<Handle<T>>`

Persists a new object and returns its `Handle<T>` directly — this is the
other (and more common) way to obtain a handle, alongside `get<T>`. See
[reference/object-model.md](object-model.md) and
[DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapters 3–4 for the
transaction context this requires.

### 3.3 `Database::materialize<T>(const Handle<T>&)` → `Result<T>`

`database.hpp:493-...` — the full decode step both `get<Member>` and
`set<Member>` call internally. Calling it yourself once and reading/writing
several fields off the resulting local `T` (then calling `update()` once) is
strictly cheaper than several separate `get<Member>`/`set<Member>` calls.

### 3.4 `Database::update<T>(Transaction&, const Handle<T>&, const T&)` → `Result<void>`

`database.hpp:581` — rewrites the entire stored object from the given value.
This is what `Handle<T>::set` calls after mutating one field; call it
directly when you've already got a fully mutated `T` in hand (e.g. from your
own `materialize()` call).

### 3.5 `Database::register_migration`

`database.hpp:741`

```cpp
using Migration = std::function<Result<FieldValues>(const DecodedObject&)>;
Result<void> register_migration(std::string type_name, std::uint64_t from_type_id, Migration fn);
```

Custom projection logic for a specific old `TypeDefinitionId`, beyond what a
declared default value in `BindingBuilder::field<Id>(name, member, default)`
can express.

## 4. Semantics & Invariants

- `Handle<T>` has exactly two data members (`DatabaseId`, `ObjectId`); it
  never owns or caches decoded data.
- Only `Database` can construct a `Handle<T>` (private constructor +
  `friend class Database`).
- `get<&T::member>()` and `set<&T::member>(tx, value)` always fully
  materialize/rewrite the object — there is no partial-field I/O path.
- `set` fails immediately (`invalid_argument`) if the given `Transaction`
  belongs to a different `Database` than the handle does.
- Resolution to the live `Database` happens via `DatabaseRegistry::find()`
  on every call, not once at handle-creation time.
- Reading an old-shaped object through a newer binding applies declared
  defaults / registered migrations on the fly; it does not rewrite the
  stored bytes.

## 5. Common Pitfalls

- **Assuming `set<&Member>` is cheap.** It is O(size of `T`), not O(1),
  regardless of how small the field you're setting is.
- **Calling `get<&Member>`/`set<&Member>` many times in a row for the same
  object.** Each call independently materializes the whole object. If you
  need several fields, call `materialize()` once yourself.
- **Holding onto a `Handle<T>` across a `detach()` of its database and being
  surprised it "still exists" as a C++ object.** It does — it's just two
  integers — but every operation on it will now fail.
- **Passing a `Transaction` from a different `Database` to `set()`.** Caught
  immediately, but worth knowing it's checked explicitly rather than being
  undefined behavior.

## 6. Worked Example

Condensed from `examples/server/by_phase/phase_03/handle_update.cpp`:

```cpp
auto tx = database->begin();
auto account = database->create(*tx, Account{"Alice", 100});
if (!account || !account->set<&Account::balance>(*tx, 125) || !tx->commit()) {
    std::cerr << "failed to update Account\n";
    return 1;
}

// Later, in any session with the type bound:
auto current = database->materialize(*database->get<Account>(account->id()));
// current->balance == 125
```

Batching multiple field changes into one round trip instead of one `set`
call per field (`Account` here is the same two-field struct as above —
`owner` and `balance`):

```cpp
auto handle = database->get<Account>(id);
auto tx = database->begin();
auto value = database->materialize(*handle);   // one decode
value->balance += 25;
value->owner = "Alice Smith";
database->update(*tx, *handle, *value);         // one rewrite, both fields
tx->commit();
```

## 7. Related Documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 5
- [reference/object-model.md](object-model.md) — schema evolution, default
  values, `BindingBuilder`
- [reference/database-lifecycle.md](database-lifecycle.md) — `DatabaseRegistry`
  resolution and attach/detach lifetime

## 8. Related Source

- `include/modb/object/handle.hpp`
- `include/modb/object/database.hpp` (`get<T>`, `materialize`, `update`,
  `register_migration`, `Handle<T>::get`/`set`)
- `examples/server/by_phase/phase_03/handle_update.cpp`
- `tests/schema_evolution_test.cpp`
