# Object Model & Catalog

> **Status:** 🚧 skeleton — outline only, not yet written. In the meantime see
> [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 2 (binding) for a
> narrative introduction.

## Overview

- TODO: what "binding a C++ type" means and why field ids (not names) are the
  durable, on-disk contract.
- TODO: how a bound type relates to the catalog (`TypeDefinition`,
  `BaselineId`) persisted inside the database file itself.

## Core API surface

- TODO `BindingBuilder<T>` — `include/modb/object/binding.hpp:197-282`
  - `field<Id>(name, &T::member)` — `binding.hpp:203-206`
  - `field<Id>(name, &T::member, default_value)` — `binding.hpp:209-215`
    (schema evolution, see below)
  - `embedded<Id>(name, &T::member, child_binding)` — `binding.hpp:219-251`
  - `.build()` — `include/modb/object/binding_builder.inl:11-36` (validation:
    rejects id 0, duplicate ids/names, empty field list)
- TODO `Binding` — `binding.hpp:166-194`
- TODO `FieldBinder` — `binding.hpp:136-151`
- TODO `attribute_type_of<Member>()` — compile-time member-type → persisted
  `AttributeType` mapping, `static_assert`s on unsupported types
- TODO `Database::bind<T>(BindingBuilder<T>)` — `include/modb/object/database.hpp:401-429`
  (must run outside any open transaction; reconciles with what's on disk —
  identical shape adopts, different shape registers a new version)
- TODO `FieldId`, `TypeDefinitionId`, `BaselineId`, `ObjectId` —
  `include/modb/object/ids.hpp`
- TODO `TypeDefinition`, catalog persistence (`DatabaseRoot`, meta-types 1–3)

## Semantics & invariants

- TODO: field ids are never reused across schema versions (ADR-001).
- TODO: the same C++ type cannot be bound twice on one `Database` instance.
- TODO: bindings are per-process, in-memory — every process that opens the
  file must call `bind()` again.
- TODO: unsupported member types fail to compile, not at runtime.

## Common pitfalls

- TODO: renumbering a field id vs. renaming it (free) — consequence for
  existing data.
- TODO: forgetting to re-bind after `Database::open()`.

## Related documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 2
- [../../arquitetura.md](../../arquitetura.md) — product vision for the object
  model
- [ADR-001](../decisions/ADR-001-identidade.md), [ADR-002](../decisions/ADR-002-bootstrap-do-catalogo.md),
  [ADR-003](../decisions/ADR-003-tipos-e-encoding.md)

## Related source

- `include/modb/object/binding.hpp`, `binding_builder.inl`
- `include/modb/object/type_definition.hpp`, `type_registry.hpp`, `baseline.hpp`
- `examples/server/by_phase/phase_01/bind_type.cpp`
- `tests/schema_evolution_test.cpp`
