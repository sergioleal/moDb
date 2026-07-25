# Lesson 4 — Handles and Updates

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

A proper "give this specific employee a raise" feature built on `Handle<T>`
instead of re-fetching by id every time, plus your first taste of schema
evolution: adding a `country` field to `Employee` with a default, without
touching existing records.

## New Concepts

- `Handle<T>`, `get<&T::member>()`/`set<&T::member>(tx, value)`, why both
  fully materialize/rewrite the object — see
  [docs/reference/handles.md](../../../reference/handles.md).
- Schema evolution: a field with a default value, lazy per-object migration
  — see
  [docs/reference/object-model.md §2.4](../../../reference/object-model.md#24-schema-evolution-and-lazy-migration).

## Starting Point

The persistent database file after Lessons 1-3 have run.

## Steps

- Look Ana up by name (via Lesson 2's index).
- Introduce `EmployeeV2` with a new `country` field defaulted to `"BR"`,
  bound under the same catalog name `"Employee"`; the file already has
  2-field records from Lessons 2-3, so this re-`bind()` triggers real
  schema evolution (a divergent-shape re-`bind()`, not a simulated one).
- Read Ana back through the new binding and confirm `country` comes from
  the declared default, not from disk.
- Raise Ana's salary via `Handle<Employee>::set<&Employee::salary>(tx,
  new_salary)` instead of the manual materialize-mutate-`update()` pattern
  from Lesson 3.

## Full Listing (End of Lesson)

[lesson_04_handles.cpp](lesson_04_handles.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_04
.\build\debug\employee_directory_lesson_04.exe
```

## Expected Output

```
Objective: evolve the schema and update through a typed Handle.
Ana read through the new binding = Ana (11250, country=BR) -- country came from the declared default, not from disk
Raise for Ana via Handle::set: Ana (13250, country=BR) -- now physically stored in the new 3-field shape
```

## What to Notice

- `Handle<T>` is pure identity — every `get`/`set` still round-trips
  through the whole object; it isn't a cache.
- The old record is not rewritten on disk just by being read with the new
  binding — only a subsequent write physically migrates it.

## Related Reference

- [docs/reference/handles.md](../../../reference/handles.md)
- [docs/reference/object-model.md](../../../reference/object-model.md)
- [DEVELOPER_GUIDE.md, Chapter 5](../../../DEVELOPER_GUIDE.md#5-handlet-and-typed-updates)
