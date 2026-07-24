# Lesson 1 — Binding Your First Type

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

The very first piece of the employee directory: an `Employee` struct and the
`BindingBuilder` that maps it into Ring0's catalog. By the end of this
lesson the program does nothing more than prove the type is known to a
database — no records exist yet (that's Lesson 2).

## New Concepts

- `BindingBuilder<T>`, field ids as the durable contract — see
  [docs/reference/object-model.md](../../reference/object-model.md).
- `Database::create`, attaching to `DatabaseRegistry` — see
  [docs/reference/database-lifecycle.md](../../reference/database-lifecycle.md)
  (this lesson only needs the very first section of it).

## Starting Point

None — this is the first lesson. You start from an empty `.cpp` file.

## Steps

- TODO: define `Employee` (`name`, `salary`, maybe `hired_on`).
- TODO: write `employee_binding()` returning a `BindingBuilder<Employee>`.
- TODO: `Database::create`, wrap in `shared_ptr`, attach to the registry.
- TODO: call `bind()`, print the resulting `TypeDefinitionId`.
- TODO: clean up (detach, remove the temp file).

## Full Listing (End of Lesson)

TODO — target: `examples/employee_directory/lesson_01_binding.cpp`.

## Build and Run

TODO — target name once the CMake entry exists:

```powershell
cmake --build --preset debug --target employee_directory_lesson_01
.\build\debug\employee_directory_lesson_01.exe
```

## Expected Output

TODO (should resemble `examples/server/by_phase/phase_01/bind_type.cpp`'s
output shape: a printed `TypeDefinitionId`).

## What to Notice

- TODO: the field id, not the field name, is what's actually persisted —
  this is the single idea the rest of the course leans on whenever the
  `Employee` struct grows.
- TODO: nothing was written to disk yet in a durable sense — no
  transaction has been opened. That's next.

## Related Reference

- [docs/reference/object-model.md](../../reference/object-model.md)
- [DEVELOPER_GUIDE.md, Chapter 2](../../DEVELOPER_GUIDE.md#2-your-first-type-bindingbuildert)
