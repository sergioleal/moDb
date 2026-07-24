# Relationships and Collections

> Part of the `docs/reference/` set — see [docs/README.md](../README.md) for
> the full index and [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md) for a
> narrative, tutorial-style introduction. This document is a precise
> reference: it assumes you already know C++ well and want exact behavior,
> not a learning path.

## 1. Overview

Ring0 has exactly three ways to relate objects, plus persistent collection
types that sit alongside them. All three relationship markers are plain
template structs wrapping an `ObjectId` (or, for `Embedded`, a value) — there
is no proxy object, no lazy-loading wrapper, no ORM-style relationship
descriptor. What each marker means is entirely determined by which one you
pick and a single boolean flag the binding layer derives from it.

## 2. Concepts

### 2.1 The three relationship kinds

`include/modb/object/ref.hpp:8-32`

```cpp
// Association (◇): holds the target's ObjectId, resolved on demand.
// Removing the target is allowed; later resolution fails detectably (ADR-008).
template <typename T> struct Ref { ObjectId target{}; };

// Composition (◆): same representation as Ref, but removing the parent
// removes the child in cascade. The association/composition distinction
// lives in AttributeDefinition.is_owned, not in the wire format.
template <typename T> struct OwnedRef { ObjectId target{}; };

// No identity of its own: serialized inline in the parent's payload.
template <typename T> struct Embedded { T value{}; };
```

`Ref<T>` and `OwnedRef<T>` are **byte-for-byte identical on disk** — both
persist as a single `ObjectId` under the `ref` attribute tag
(§[object-model.md §2.3](object-model.md#23-compile-time-member-type-mapping)).
The only thing that distinguishes composition from association is
`AttributeDefinition::is_owned`, set from `is_owned_ref_v<Member>` at bind
time (`binding.hpp:264`). This is a deliberate simplification: cascade
behavior is metadata about the *field*, not a different runtime
representation.

`Embedded<T>` is structurally different: it requires its own nested
`Binding` at bind time and is encoded inline inside the parent's own payload
(`field_count u16 | (field_id u16, value)*`, `encode_embedded`/
`decode_embedded`). It has no `ObjectId` and can never be the target of
`Database::get<T>`.

### 2.2 Cascade delete, cycle detection, and dangling `Ref`s

```cpp
// src/object/database.cpp:627-665
Result<void> Database::remove_cascade(ObjectId id, std::unordered_set<std::uint64_t>& visited) {
    if (!visited.insert(id.value).second) {
        return std::unexpected(
            Error{ErrorCode::invalid_argument, "cycle detected in owned-reference cascade"});
    }
    auto object = store_.get(id);
    auto type = store_.find_type(object->type);
    // Depth-first: remove OwnedRef children first, then the object itself —
    // so a failure partway through never leaves a parent orphaned of children.
    for (const auto& attribute : type->get().attributes()) {
        if (!attribute.is_owned) continue;
        for (const auto& [field_id, value] : object->fields) {
            if (field_id != attribute.id || value.is_null()) continue;
            auto target = value.as_ref();
            if (target->value != 0) {
                if (auto removed = remove_cascade(*target, visited); !removed) {
                    return std::unexpected(removed.error());
                }
            }
        }
    }
    return store_.remove(id, oldest_open_snapshot_epoch());
}
```

Reading this precisely: `Database::remove(tx, id)` walks every field the
binding marked `is_owned` (i.e. every `OwnedRef<T>` member), recurses into
each target *before* removing the current object (depth-first, children
first), and tracks visited ids in the current recursion so that a
composition cycle (`A` owns `B` owns `A`) is **rejected with
`invalid_argument` before anything is deleted** — not an infinite loop, not
silent data loss.

Plain `Ref<T>` fields are never touched by this walk. If you remove an
object that other objects hold a `Ref<T>` to, those references are now
dangling on purpose — resolving them later (materializing the referencing
object and following the `Ref`) fails with `record_not_found`. This is
expected, detectable state under ADR-008, not corruption: Ring0 does not
maintain automatic bidirectional integrity for plain associations.

### 2.3 Collections are views over a `BlobId`, not native members

`PersistentVector<T>`, `PersistentSet<T>`, `PersistentMap<K, V>`
(`include/modb/object/collection.hpp`) are **not** things you declare with
`BindingBuilder::field`. The object that "owns" a collection only stores a
plain `BlobId` field; the collection object itself is a thin, stateless
handle constructed on demand from a `BlobStore&` and that `BlobId`:

```cpp
auto blobs = database->blobs();                                    // database.hpp:977
auto projects = PersistentVector<Ref<Project>>::create(blobs, *tx); // allocates an empty blob
projects->push_back(*tx, Ref<Project>{some_project_id});
employee.projects = projects->id();  // YOU persist the BlobId on the parent
```

Reopening later is symmetric — construct a new view over the same id:

```cpp
PersistentVector<Ref<Project>> reopened{blobs, employee.projects};
reopened.for_each([](const Ref<Project>& ref) { /* ... */ return Result<void>{}; });
```

`Database::blobs()` returns a `BlobStore` by value
(`BlobStore{*file_, database_id_, true}`, `database.hpp:977`) — cheap to
call repeatedly, not something you need to cache yourself.

### 2.4 Every collection mutation rewrites the whole blob (MVP limitation)

Look at `PersistentVector<T>::push_back` (`collection.hpp:145-172`): it reads
the *entire* existing blob, decodes nothing (it reuses the raw bytes after
the count), appends the new encoded element, and calls
`blobs_->rewrite(id_, ...)` — a full rewrite, not an in-place append.
`PersistentSet::insert` and `PersistentMap::put`/`remove` do the same:
locate the insertion point via binary search over already-encoded element
ranges (kept sorted by canonical byte encoding, `encoded_less`), then rebuild
the entire blob with the element inserted/replaced/removed. This is `O(n)`
per mutation, explicitly called out in the source as an MVP limitation, not
an oversight — fine for small, bounded collections; not an append-optimized
structure for large ones.

`PersistentSet<T>`/`PersistentMap<K,V>` keep their entries **sorted by the
canonical encoded byte representation** of the element/key (`encoded_less`,
lexicographic byte comparison) — reads use binary search
(`contains`/`get`), but every write still touches the whole blob to keep
that order.

## 3. API Reference

### 3.1 `require_collection_transaction`

`include/modb/object/collection.hpp:46-61` — called at the start of every
mutating collection method. Fails with:
- `invalid_argument` if the `Transaction` isn't attached to any database, or
  belongs to a different database than the `BlobStore`'s owner.
- `transaction_required` if the backing file has no active write
  transaction.

A "raw" `BlobStore` with no owner (`owner().value == 0`) is reserved for
diagnostics and skips the owner check — this is what CLI tools like `modb blob`
use, not application code.

### 3.2 `PersistentVector<T>`

```cpp
PersistentVector(BlobStore& blobs, BlobId id) noexcept;                       // open existing
static Result<PersistentVector> create(BlobStore& blobs, Transaction& tx);      // allocate empty
[[nodiscard]] BlobId id() const noexcept;
[[nodiscard]] Result<std::size_t> size() const;
[[nodiscard]] Result<T> at(std::size_t index) const;                           // O(index) — see §5
[[nodiscard]] Result<void> push_back(Transaction& tx, const T& value);         // O(n)
[[nodiscard]] Result<void> for_each(const std::function<Result<void>(const T&)>&) const;
```

### 3.3 `PersistentSet<T>`

```cpp
PersistentSet(BlobStore& blobs, BlobId id) noexcept;
static Result<PersistentSet> create(BlobStore& blobs, Transaction& tx);
[[nodiscard]] BlobId id() const noexcept;
[[nodiscard]] Result<std::size_t> size() const;
[[nodiscard]] Result<void> insert(Transaction& tx, const T& value);  // dedup, keeps sort order
[[nodiscard]] Result<bool> contains(const T& value) const;           // binary search
[[nodiscard]] Result<void> for_each(const std::function<Result<void>(const T&)>&) const;
```

### 3.4 `PersistentMap<K, V>`

```cpp
PersistentMap(BlobStore& blobs, BlobId id) noexcept;
static Result<PersistentMap> create(BlobStore& blobs, Transaction& tx);
[[nodiscard]] BlobId id() const noexcept;
[[nodiscard]] Result<std::size_t> size() const;
[[nodiscard]] Result<void> put(Transaction& tx, const K& key, const V& value); // insert or replace
[[nodiscard]] Result<std::optional<V>> get(const K& key) const;                 // binary search
[[nodiscard]] Result<bool> remove(Transaction& tx, const K& key);               // true if it existed
```

### 3.5 `Database::remove(Transaction&, ObjectId)`

`include/modb/object/database.hpp:963` → calls `remove_cascade` (§2.2)
internally. Requires an active write transaction on the same database
(same `check_writable` path as every other mutation).

## 4. Semantics & Invariants

- `Ref<T>` and `OwnedRef<T>` are wire-identical; only `is_owned` (bind-time
  metadata) distinguishes them.
- `Database::remove` recurses into `OwnedRef` children depth-first, children
  before parent; a composition cycle is rejected (`invalid_argument`)
  **before** any deletion happens.
- Plain `Ref<T>` targets are never followed on delete — dangling references
  resolve to `record_not_found` later, by design (ADR-008), not corruption.
- `Embedded<T>` has no `ObjectId` and can never be fetched independently via
  `Database::get`.
- Every `PersistentVector`/`Set`/`Map` mutation rewrites the entire
  underlying blob (`O(n)`), and the collection object's `BlobId` **changes**
  after a mutation — you must re-store the id returned by
  `push_back`/`insert`/`put`/`remove` on the owning object if you want the
  update to actually be visible next time (the parent object only knows
  about the id it was given; the collection object updates its own `id_`
  member in place, but that new id needs to reach persisted storage via
  the parent's own field).
- `PersistentSet`/`PersistentMap` keep entries sorted by canonical encoded
  bytes, not by any user-defined comparator.

## 5. Common Pitfalls

- **Forgetting to persist the (possibly new) `BlobId` on the parent object
  after a mutation.** Every mutating collection call can change `id()` —
  read it back and store it via `Handle::set`/`Database::update` on the
  owning object, or the next reopen will see stale/no data.
- **Assuming `PersistentVector::at(index)` is O(1).** It isn't — it decodes
  and skips every preceding element (`collection.hpp:123-143`); prefer
  `for_each` for full traversal instead of a `size()`+`at(i)` loop.
- **Mutating a collection without an active transaction from the right
  database.** Caught explicitly by `require_collection_transaction`, but
  worth knowing it's checked, not assumed.
- **Expecting a removed `Ref<T>` target to be caught automatically.** It
  isn't — only `OwnedRef` cascades; plain `Ref` requires you to handle
  `record_not_found` at resolution time.

## 6. Worked Example

Association + composition, from `examples/server/by_phase/phase_04/relationships.cpp`:

```cpp
struct Employee {
    std::string name;
    modb::object::Ref<Department> department{};
    modb::object::OwnedRef<Badge> badge{};
};
builder.field<1>("name", &Employee::name)
    .field<2>("department", &Employee::department)
    .field<3>("badge", &Employee::badge);

auto tx = database->begin();
auto department = database->create(*tx, Department{"Engineering"});
auto badge = database->create(*tx, Badge{7});
auto employee = database->create(*tx, Employee{"Ana", {department->id()}, {badge->id()}});
tx->commit();

// Removing the employee cascades into the OwnedRef<Badge> (badge is deleted
// too); the Ref<Department> target survives untouched.
auto tx2 = database->begin();
database->remove(*tx2, employee->id());
tx2->commit();
// database->get<Department>(department->id()) still succeeds.
// database->get<Badge>(badge->id()) now fails: record_not_found.
```

Embedded value + a `Ref`-typed collection, adapted from `tests/relationship_test.cpp`
and `tests/collection_test.cpp`:

```cpp
struct Address { std::string street; std::int64_t number{}; };
struct Employee {
    std::string name;
    Ref<Department> department{};
    Embedded<Address> address{};
    modb::object::BlobId projects{}; // holds a PersistentVector<Ref<Project>>
};

// ... bind Employee with .embedded<3>("address", &Employee::address, address_binding()) ...

auto tx = database->begin();
auto blobs = database->blobs();
auto projects = PersistentVector<Ref<Project>>::create(blobs, *tx);
projects->push_back(*tx, Ref<Project>{lead_project_id});

Employee ana{"Ana", Ref<Department>{dept_id}, Embedded<Address>{{"Rua das Flores", 10}},
             projects->id()};
auto handle = database->create(*tx, ana);
tx->commit();
```

## 7. Related Documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 6
- [reference/object-model.md](object-model.md) — `Embedded<T>` binding,
  `attribute_type_of`
- [ADR-008](../decisions/ADR-008-integridade-de-referencias.md) — cascade and
  reference-integrity semantics

## 8. Related Source

- `include/modb/object/ref.hpp`, `collection.hpp`, `blob_store.hpp`
- `include/modb/object/database.hpp` (`remove`, `remove_cascade`, `blobs()`)
- `examples/server/by_phase/phase_04/relationships.cpp`
- `tests/relationship_test.cpp`, `tests/collection_test.cpp`
