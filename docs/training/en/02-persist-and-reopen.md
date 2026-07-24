# Lesson 2 — Persist and Reopen

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

Real employee records that survive closing and reopening the directory. The
program now writes a handful of employees in one run, and a second run
(simulating a restart) reads them back by id.

## New Concepts

- `Database::create` vs `Database::open`, the `shared_ptr` +
  `DatabaseRegistry` attach/detach pattern — see
  [docs/reference/database-lifecycle.md](../../reference/database-lifecycle.md).
- Why bindings must be repeated on every process start (they don't persist
  themselves).

## Starting Point

Lesson 1's `Employee` binding and database creation.

## Steps

- TODO: split `main()` into two lifetimes ("first run" / "second run") in
  the same process, mirroring two separate program invocations.
- TODO: first lifetime: create, bind, begin a transaction, create a few
  `Employee` records, commit, detach.
- TODO: second lifetime: open (not create), re-bind, read one record back
  by the `ObjectId` captured earlier.
- TODO: print before/after to make the round-trip visible.

## Full Listing (End of Lesson)

TODO — target: `examples/employee_directory/lesson_02_persist_reopen.cpp`.

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_02
.\build\debug\employee_directory_lesson_02.exe
```

## Expected Output

TODO — should show the same employee name/salary printed twice, once
right after creation and once after the simulated restart.

## What to Notice

- TODO: forgetting to re-`bind()` after `open()` is the most common mistake
  at this stage — call it out explicitly with a broken/fixed comparison.
- TODO: the `shared_ptr`/registry attach isn't ceremony — it's why a
  `Handle` obtained in the first lifetime would be meaningless in the
  second (different registry id, even same file).

## Related Reference

- [docs/reference/database-lifecycle.md](../../reference/database-lifecycle.md)
- [DEVELOPER_GUIDE.md, Chapter 3](../../DEVELOPER_GUIDE.md#3-create-persist-reopen)
