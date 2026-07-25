# Lesson 1 — Binding Your First Type

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

The very first piece of the employee directory: an `Employee` struct and the
`BindingBuilder` that maps it into Ring0's catalog. By the end of this
lesson the program does nothing more than prove the type is known to a
database — no records exist yet (that's Lesson 2).

## New Concepts

- `BindingBuilder<T>`, field ids as the durable contract — see
  [docs/reference/object-model.md](../../../reference/object-model.md).
- `Database::create`, attaching to `DatabaseRegistry` — see
  [docs/reference/database-lifecycle.md](../../../reference/database-lifecycle.md)
  (this lesson only needs the very first section of it).

## Starting Point

None — this is the first lesson. You start from an empty `.cpp` file.

## Steps

- Define `Employee` (`name`, `salary`).
- Write `employee_binding()` returning a `BindingBuilder<Employee>`.
- `Database::create`, wrap in `shared_ptr`, attach to the registry.
- Call `bind()`, print the resulting `TypeDefinitionId`.
- Detach from the registry — but leave the file itself in place. Every
  later lesson keeps reopening this same file, so nothing gets deleted
  until the very end of the whole run.

## Full Listing (End of Lesson)

[lesson_01_binding.cpp](lesson_01_binding.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_01
.\build\debug\employee_directory_lesson_01.exe
```

## Expected Output

```
Objective: bind an Employee type and register it in the catalog.
Lesson 1: Employee type id = 16
```

The exact id depends on how many catalog-internal types are registered
ahead of it; treat the number as "some small positive id," not a promise
of `16` specifically.

## What to Notice

- The field id, not the field name, is what's actually persisted — this
  is the single idea the rest of the course leans on whenever the
  `Employee` struct grows.
- Nothing was written to disk yet in a durable sense — no
  transaction has been opened. That's next.

## Related Reference

- [docs/reference/object-model.md](../../../reference/object-model.md)
- [DEVELOPER_GUIDE.md, Chapter 2](../../../DEVELOPER_GUIDE.md#2-your-first-type-bindingbuildert)
