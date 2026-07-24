# Object Model & Catalog

> Part of the `docs/reference/` set — see [docs/README.md](../README.md) for
> the full index and [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md) for a
> narrative, tutorial-style introduction. This document is a precise
> reference: it assumes you already know C++ well and want exact behavior,
> not a learning path.

## 1. Overview

Ring0 does not use reflection, macros, or code generation to persist a C++
type. You describe the mapping explicitly, once, with `BindingBuilder<T>`: a
small fluent builder that ties each C++ data member to a **stable numeric
id** (`FieldId`) and produces an immutable `Binding` — a type-erased
description of how to read and write `T` through `void*`.

That `Binding` is registered into the database's **catalog** — and the
catalog itself is not special-cased storage: it is made of ordinary objects
(`TypeDefinition`, `Baseline`), persisted the same way any other object is,
bootstrapped by three reserved meta-type ids compiled into the engine. This
is a deliberate design choice (ADR-002): the catalog can evolve, be
inspected, and be recovered using the exact same machinery as user data.

The two central ideas a newcomer needs before anything else makes sense:

1. **The `FieldId` — not the field name — is the durable, on-disk contract.**
   Names are for humans; ids are what the storage engine actually persists
   and what the decoder uses to reassemble an object. An id is never
   reused across schema versions of the same type (ADR-001).
2. **A `Binding` is not a template.** `BindingBuilder<T>` *is* a template
   (it needs to know `T` to take pointer-to-members), but the `Binding` it
   produces is a plain, non-template class. The dependency on `T` is fully
   encapsulated inside per-field `std::function` accessors that operate on
   `void*`. This is what lets the rest of the engine (codec, `ProjectionPlan`,
   catalog) work with any bound type without being templated on it.

## 2. Concepts

### 2.1 Field ids as the wire contract

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
(`examples/server/by_phase/phase_01/bind_type.cpp:13-23`)

`field<Id>(name, &T::member)` is a member function template where `Id` is a
**non-type template parameter** (`std::uint16_t`). It is resolved entirely at
compile time — there is no runtime lookup involved in choosing which id maps
to which member. Renaming `"score"` to `"points"` costs nothing: the string
is cosmetic, used only for diagnostics and for `AttributeDefinition::name`
lookups. Changing `field<2>` to `field<3>` for the same member, on the other
hand, makes the storage engine treat it as an entirely different attribute:
existing rows still have a value under id `2`, and the new binding no longer
reads it.

### 2.2 Catalog as objects, bootstrapped by reserved ids

```cpp
// include/modb/object/ids.hpp:76-84
inline constexpr ObjectId meta_type_definition{1};
inline constexpr ObjectId meta_attribute_definition{2};
inline constexpr ObjectId meta_baseline{3};
inline constexpr std::uint64_t first_user_object_id = 16;
```

`ObjectId`s 1–15 are reserved for the catalog (ADR-002); ids 1–3 identify the
three meta-types the engine already knows how to decode without reading
anything from disk first — this is what lets the catalog bootstrap itself
from an otherwise-empty file. Everything an application defines (its own
`TypeDefinition`s, its own objects) starts at id 16.

- `TypeDefinition` (`include/modb/object/type_definition.hpp:59-103`) — an
  immutable, validated description of a type: a name and an ordered list of
  `AttributeDefinition`. Unlike a relational `Schema`, a `TypeDefinition`
  with zero attributes is degenerate but valid (a pure marker type).
- `Baseline` (`include/modb/object/baseline.hpp:26-60`) — an immutable
  snapshot of *all* `TypeDefinitionId`s valid at one point in time. A new
  baseline is created whenever a bound type's shape changes (see §2.4);
  older baselines remain loadable so historical objects can still be
  interpreted correctly.
- `TypeDefinitionId` and `BaselineId` are both literally `ObjectId` (type
  aliases, `ids.hpp:68-69`) — the catalog's own identity **is** the
  `ObjectId` of the object that represents it. There is no separate id
  space for metadata.

### 2.3 Compile-time member-type mapping

`attribute_type_of<Member>()` (`include/modb/object/binding.hpp:47-69`) is a
`consteval` function that maps a C++ member type to the persisted
`AttributeType` it will be stored as:

```cpp
enum class AttributeType : std::uint8_t {
    null = 0, boolean = 1, int64 = 2, float64 = 3,
    string = 4, bytes = 5, ref = 6, blob = 7, embedded = 8,
};
```
(`include/modb/object/attribute_value.hpp:28-38`)

`bool → boolean`, any `std::signed_integral → int64`, `double → float64`,
`std::string → string`, `std::vector<std::byte> → bytes`, `BlobId → blob`,
and — notably — **both** `Ref<T>` and `OwnedRef<T>` map to the same tag,
`ref` (`binding.hpp:59-60`); the association/composition distinction lives
in a separate `FieldBinder::is_owned` flag, not in the attribute type. Any
member type that doesn't match one of these falls into a `static_assert`
(`binding.hpp:66-67`) — **a compile error, not a runtime one**. `Embedded<T>`
members are deliberately excluded from this mapping; they go through the
dedicated `.embedded<Id>(...)` builder method instead (§2.5).

### 2.4 Schema evolution and lazy migration

A field can declare a default value, used when reading an older record that
doesn't have it:

```cpp
struct EmployeeV1 { std::string name; double salary{}; };
struct EmployeeV2 { std::string name; double salary{}; std::string country; };

BindingBuilder<EmployeeV1> employee_v1() {
    BindingBuilder<EmployeeV1> builder{"Employee"};
    builder.field<1>("name", &EmployeeV1::name).field<2>("salary", &EmployeeV1::salary);
    return builder;
}
BindingBuilder<EmployeeV2> employee_v2() {
    BindingBuilder<EmployeeV2> builder{"Employee"};
    builder.field<1>("name", &EmployeeV2::name)
        .field<2>("salary", &EmployeeV2::salary)
        .field<3>("country", &EmployeeV2::country, "BR"); // default for old rows
    return builder;
}
```
(`tests/schema_evolution_test.cpp:36-59`)

`Database::bind` (§3.4) reconciles a builder's `Binding` with whatever is
already registered for that type name:

```cpp
// src/object/database.cpp:287-302
Result<TypeDefinitionId> Database::register_or_adopt(const Binding& binding) {
    auto canonical = binding.to_type_definition();
    if (auto existing = store_.find_type(binding.type_name())) {
        if (same_structure(existing->get(), *canonical)) {
            return existing->get().id();       // identical shape -> adopt
        }
        return store_.register_type(std::move(*canonical)); // divergent -> new version
    }
    return store_.register_type(std::move(*canonical));      // brand new
}
```

If the shape is identical, the existing `TypeDefinitionId` is adopted — no
new catalog entry. If it diverges (as `EmployeeV2` does from `EmployeeV1`), a
**new `TypeDefinition` and a new `Baseline`** are registered; the previous
baseline stays around, unchanged, so a session that reopens the file and
reads an `EmployeeV1` row through the *old* binding still gets a correct,
historically-accurate decode. Reading an old row through the *new* binding
(`EmployeeV2`) fills the missing `country` field from the declared default —
this happens through a `ProjectionPlan` built once per stored
`TypeDefinitionId` and cached (`ProjectionPlan::build_count()` in the test
stays at `1` across repeated reads of the same old-shaped object,
`schema_evolution_test.cpp:141-150`). **The old row is not rewritten on disk**
just by being read this way — it only gets physically rewritten the next
time something calls `update()` on it (e.g. through `Handle::set`, see
[reference/handles.md](handles.md)).

For conversions that aren't a simple default value,
`Database::register_migration(...)` accepts a custom function from the old
decoded shape (`FieldValues`) to whatever the new binding expects — this is
the escape hatch for anything default-value substitution can't express
(renamed semantics, computed fields, unit conversions, etc.).

### 2.5 Embedded sub-objects

`Embedded<T>` members have no identity of their own — they are serialized
*inline*, inside the parent's own payload, using a small self-contained
sub-format (`field_count u16 | (field_id u16, value)*`,
`encode_embedded`/`decode_embedded` in `binding.hpp:153-157`). They need
their own nested `Binding`, supplied at bind time:

```cpp
struct Address {
    std::string street;
    std::int64_t number{};
};
struct Employee {
    std::string name;
    Ref<Department> department{};
    Embedded<Address> address{};
};

Binding address_binding() {
    BindingBuilder<Address> builder{"Address"};
    builder.field<1>("street", &Address::street).field<2>("number", &Address::number);
    return std::move(*builder.build()); // note: already-built Binding, not a builder
}

BindingBuilder<Employee> employee_builder() {
    BindingBuilder<Employee> builder{"Employee"};
    builder.field<1>("name", &Employee::name)
        .field<2>("department", &Employee::department)
        .embedded<3>("address", &Employee::address, address_binding());
    return builder;
}
```
(`tests/relationship_test.cpp:50-92`)

Because an `Embedded<T>` value has no `ObjectId`, it can never be the target
of `Database::get<T>` — it only exists as part of whatever object owns it.

## 3. API Reference

### 3.1 `BindingBuilder<T>`

`include/modb/object/binding.hpp:197-282`

```cpp
template <typename T>
class BindingBuilder {
public:
    explicit BindingBuilder(std::string type_name);

    template <std::uint16_t Id, typename Member>
    BindingBuilder& field(std::string name, Member T::* member);

    template <std::uint16_t Id, typename Member, typename Default>
        requires std::constructible_from<Member, Default>
    BindingBuilder& field(std::string name, Member T::* member, Default&& default_value);

    template <std::uint16_t Id, typename U>
    BindingBuilder& embedded(std::string name, Embedded<U> T::* member, Binding child);

    [[nodiscard]] Result<Binding> build();
};
```

- `type_name` — the catalog-visible name of the type (independent of the C++
  class name; two different C++ types could in principle share a
  `type_name`, though doing so on purpose would be unusual).
- `field<Id>(name, member)` — binds a plain member; the `AttributeType` is
  derived from the member's C++ type via `attribute_type_of<Member>()`
  (§2.3).
- `field<Id>(name, member, default_value)` — same, plus a default used when
  projecting an older record that lacks this field (§2.4). `Default` must be
  constructible into `Member` (`std::constructible_from<Member, Default>`).
- `embedded<Id>(name, member, child)` — binds an `Embedded<U>` member using a
  separately-built child `Binding` (§2.5).
- `build()` — validates and returns the immutable `Binding`. See §3.3 for
  exactly what it rejects.

### 3.2 `Binding`

`include/modb/object/binding.hpp:166-194`

```cpp
class Binding {
public:
    [[nodiscard]] const std::string& type_name() const noexcept;
    [[nodiscard]] std::span<const FieldBinder> fields() const noexcept;
    [[nodiscard]] Result<TypeDefinition> to_type_definition() const;
    [[nodiscard]] Result<FieldValues> to_field_values(const void* object) const;
    [[nodiscard]] Result<void> materialize(const FieldValues& fields, void* destination) const;
};
```

Not a template. `fields()` exposes the underlying `FieldBinder` list
(`binding.hpp:136-151`) — each one holding an `id`, `name`, `AttributeType`,
`is_owned`/`is_embedded` flags, an optional `default_value`, and two
type-erased accessors (`std::function<Result<void>(void*, const AttributeValue&)> store`
and `std::function<Result<AttributeValue>(const void*)> load`). This is the
mechanism that lets `ProjectionPlan` and the codec operate on any bound type
without being templated themselves.

### 3.3 Validation performed by `build()`

`include/modb/object/binding_builder.inl:11-36`. `build()` fails
(`Result<Binding>` carrying an `Error`) when:

| Condition | `ErrorCode` |
|---|---|
| No fields were added | `invalid_argument` |
| A field's `Id` is `0` | `invalid_argument` |
| Two fields share the same `Id` | `duplicate_field` |
| Two fields share the same name | `duplicate_column` |

### 3.4 `Database::bind<T>`

`include/modb/object/database.hpp:401-429`

```cpp
template <typename T>
[[nodiscard]] Result<void> bind(BindingBuilder<T> builder);
```

Behavior, in order:

1. Fails with `transaction_active` if a write transaction is currently open
   on this `Database` — binding is catalog configuration, and runs under its
   own internal transaction (see `persist_binding`, `database.cpp:304-330`),
   not the caller's.
2. Fails with `binding_mismatch` if `T` is already bound on this `Database`
   instance.
3. Calls `builder.build()` (§3.3); propagates any validation error.
4. Persists the resulting `Binding` via `register_or_adopt` (§2.4),
   durably, through the WAL, inside its own transaction — registering or
   evolving a type is atomic and crash-safe like any other write.
5. Only *after* that internal commit succeeds does it update the in-memory
   `bound_` map for this process.

### 3.5 Related catalog queries on `Database`

- `Database::type_id_of<T>()` → `Result<TypeDefinitionId>` — the currently
  bound `TypeDefinitionId` for `T` (`database.hpp:637`).
- `Database::object_type(ObjectId)` → `Result<TypeDefinitionId>` — which type
  a specific stored object was written as (`database.hpp:841`).
- `Database::current_baseline()` → `const std::optional<Baseline>&` — the
  active baseline (`database.hpp:852-854`).
- `Database::find_baseline(BaselineId)` → historical baseline lookup
  (`database.hpp:917-919`).

## 4. Semantics & Invariants

- A `FieldId` is unique **within a type** and is never reused across schema
  versions of that type (ADR-001) — even if a field is dropped, its old id
  should not be given to an unrelated new field.
- The same C++ type cannot be bound twice on one `Database` instance
  (`binding_mismatch`).
- Bindings are **per-process, in-memory** state — nothing about "which
  builder maps to which type" is restored automatically from the file.
  Every process that opens the database must call `bind()` again with a
  builder describing the same (or an intentionally evolved) shape.
- `bind()` must run outside any open write transaction.
- Unsupported member types are a **compile-time** failure
  (`binding_unsupported_type` `static_assert`), not a runtime one.
- A divergent re-bind creates a new `TypeDefinition` + `Baseline`; it does
  **not** touch previously-written rows. Old rows are migrated lazily, one
  object at a time, only when something writes to them again.
- `ObjectId`s `1`–`15` are reserved for the catalog; application data starts
  at `16` (`first_user_object_id`).

## 5. Common Pitfalls

- **Treating the field name as the contract.** It is purely cosmetic —
  renaming is free, renumbering is not.
- **Forgetting to re-`bind()` after `Database::open()`.** Bindings don't
  survive a process restart automatically.
- **Assuming a divergent re-bind rewrites existing data.** It doesn't —
  reads get lazily projected with defaults; only writes rewrite.
- **Trying to `Database::get<T>` an `Embedded<T>` value directly.** It has no
  `ObjectId` — it only exists inside its owner's payload.
- **Reusing a dropped field's old id for something unrelated.** The engine
  won't stop you, but old, unread WAL/data pages could still carry the old
  semantics under that id.

## 6. Worked Example: Binding, Persisting, and Evolving a Type

Full scenario, condensed from `tests/schema_evolution_test.cpp`:

```cpp
// 1. Define and bind v1.
auto created = Database::create(path);
auto database = std::make_shared<Database>(std::move(*created));
auto id = DatabaseRegistry::instance().attach(database);
database->bind(employee_v1());

auto tx = database->begin();
auto ana = database->create(*tx, EmployeeV1{"Ana", 15000.0});
auto bia = database->create(*tx, EmployeeV1{"Bia", 12000.0});
const auto v1_baseline = database->current_baseline()->id();
tx->commit();
DatabaseRegistry::instance().detach(*id);

// 2. Reopen in a new "session" and bind v2 (adds `country`, default "BR").
auto opened = Database::open(path);
auto database2 = std::make_shared<Database>(std::move(*opened));
auto id2 = DatabaseRegistry::instance().attach(database2);
database2->bind(employee_v2()); // divergent shape -> new TypeDefinition + Baseline

assert(database2->current_baseline()->id() != v1_baseline);
auto historical = database2->find_baseline(v1_baseline);
assert(historical.has_value()); // the old baseline is still there, unchanged

// 3. Read an old (v1) row through the new (v2) binding.
auto ana_handle = database2->get<EmployeeV2>(ana->id());
auto ana_value = database2->materialize(*ana_handle);
assert(ana_value->name == "Ana" && ana_value->salary == 15000.0);
assert(ana_value->country == "BR"); // filled from the declared default

// 4. Old and new objects coexist in the same session.
auto tx2 = database2->begin();
auto carla = database2->create(*tx2, EmployeeV2{"Carla", 18000.0, "PT"});
tx2->commit();
```

## 7. Related Documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 2
- [reference/handles.md](handles.md) — `Handle<T>` and when a lazily-projected
  object actually gets rewritten
- [../../arquitetura.md](../../arquitetura.md) — product vision for the
  object model
- [ADR-001](../decisions/ADR-001-identidade.md) — identity (`ObjectId` and
  the other strong ids)
- [ADR-002](../decisions/ADR-002-bootstrap-do-catalogo.md) — catalog
  bootstrap (reserved meta-type ids)
- [ADR-003](../decisions/ADR-003-tipos-e-encoding.md) — attribute types and
  binary encoding

## 8. Related Source

- `include/modb/object/binding.hpp`, `binding_builder.inl`
- `include/modb/object/type_definition.hpp`, `baseline.hpp`, `ids.hpp`,
  `attribute_value.hpp`
- `include/modb/object/type_registry.hpp`, `object_codec.hpp`,
  `projection_plan.hpp`
- `examples/server/by_phase/phase_01/bind_type.cpp`
- `tests/schema_evolution_test.cpp`, `tests/binding_test.cpp`
