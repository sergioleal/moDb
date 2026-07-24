# Lesson 4 — Handles and Updates

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

A proper "give this specific employee a raise" feature built on `Handle<T>`
instead of re-fetching by id every time, plus your first taste of schema
evolution: adding a `country` field to `Employee` with a default, without
touching existing records.

## New Concepts

- `Handle<T>`, `get<&T::member>()`/`set<&T::member>(tx, value)`, why both
  fully materialize/rewrite the object — see
  [docs/reference/handles.md](../../reference/handles.md).
- Schema evolution: a field with a default value, lazy per-object migration
  — see
  [docs/reference/object-model.md §2.4](../../reference/object-model.md#24-schema-evolution-and-lazy-migration).

## Starting Point

Lesson 3's directory, with transactional raise/rollback behavior in place.

## Steps

- TODO: refactor the raise feature from Lesson 3 to take a `Handle<Employee>`
  and use `set<&Employee::salary>(tx, new_salary)` instead of
  materialize-mutate-`update()` by hand.
- TODO: add a second, unrelated field mutation (e.g. a promotion that
  changes a title/role field) to show batching two `set` calls vs.
  materializing once and calling `update()` once.
- TODO: introduce `EmployeeV2` with a new `country` field defaulted to
  something reasonable; re-bind and read an old (v1-shaped) record through
  the new binding.

## Full Listing (End of Lesson)

TODO — target: `examples/employee_directory/lesson_04_handles.cpp`.

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_04
.\build\debug\employee_directory_lesson_04.exe
```

## Expected Output

TODO — a raise applied via `Handle::set`, and an old employee record read
back with the new field filled from its default.

## What to Notice

- TODO: `Handle<T>` is pure identity — every `get`/`set` still round-trips
  through the whole object; it isn't a cache.
- TODO: the old record is not rewritten on disk just by being read with the
  new binding — only a subsequent write physically migrates it.

## Related Reference

- [docs/reference/handles.md](../../reference/handles.md)
- [docs/reference/object-model.md](../../reference/object-model.md)
- [DEVELOPER_GUIDE.md, Chapter 5](../../DEVELOPER_GUIDE.md#5-handlet-and-typed-updates)
