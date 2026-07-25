# Lesson 2 — Persist and Reopen

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

Real employee records that survive closing and reopening the directory. The
program now writes a handful of employees in one run, and a second run
(simulating a restart) reads them back by id.

## New Concepts

- `Database::create` vs `Database::open`, the `shared_ptr` +
  `DatabaseRegistry` attach/detach pattern — see
  [docs/reference/database-lifecycle.md](../../../reference/database-lifecycle.md).
- Why bindings must be repeated on every process start (they don't persist
  themselves).

## Starting Point

Lesson 1's `Employee` binding and database creation.

## Steps

- Split `main()` into two lifetimes ("first run" / "second run") in the
  same process, mirroring two separate program invocations.
- First lifetime: create, bind, begin a transaction, create a few
  `Employee` records, commit, detach.
- Second lifetime: open (not create), re-bind, read one record back by
  the `ObjectId` captured earlier.
- Print before/after to make the round-trip visible.

## Full Listing (End of Lesson)

[lesson_02_persist_reopen.cpp](lesson_02_persist_reopen.cpp)
— note this file also carries Lesson 1's `lesson_01_bind_type` function
forward and calls it first, since `main()` replays every lesson up to the
current one against one continuously-reopened file (see the course
[README](../README.md#how-the-code-is-organized)).

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_02
.\build\debug\employee_directory_lesson_02.exe
```

## Expected Output

```
Objective: persist real employees and read them back after a restart.
Lesson 1: Employee type id = 16
Lesson 2: wrote 3 employees (Ana=18, Bruno=19, Carla=20)
Lesson 2: after reopen, employee 18 = Ana (12000)
```

The object ids (18/19/20) are deterministic for this exact sequence of
operations, but they're not "2, 3, 4" — the catalog itself consumes a few
ids first (type definitions, baselines) before your first real record.

## What to Notice

- Forgetting to re-`bind()` after `open()` is the most common mistake at
  this stage — every lesson function in this course re-binds every type
  it touches, every single time it opens the file, for exactly this
  reason.
- The `shared_ptr`/registry attach isn't ceremony — it's why a `Handle`
  obtained in the first lifetime would be meaningless in the second
  (different registry id, even same file).

## Related Reference

- [docs/reference/database-lifecycle.md](../../../reference/database-lifecycle.md)
- [DEVELOPER_GUIDE.md, Chapter 3](../../../DEVELOPER_GUIDE.md#3-create-persist-reopen)
