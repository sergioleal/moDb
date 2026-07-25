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

The persistent database file Lesson 1 created (see the course
[README](../README.md#how-the-code-is-organized)) — this lesson is a
separate program run that opens it.

## Steps

- Two scoped blocks simulate two separate program runs, both inside this
  one lesson's `main()`, against the SAME file Lesson 1 created.
- First block: open (never create — Lesson 1 already did that), bind,
  begin a transaction, create three `Employee` records, commit.
- Also in the first block: `create_index<Employee>(FieldId{1})` on
  `name` — every lesson from here on is a genuinely separate binary with
  no shared in-process state, so this index is what lets them find "Ana"
  again without remembering an `ObjectId` across runs.
- Second block: open again (a fresh `Database` instance, simulating a
  restart), re-bind, look Ana up **by name** through the new index, then
  read her record back by the resulting id.

## Full Listing (End of Lesson)

[lesson_02_persist_reopen.cpp](lesson_02_persist_reopen.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_02
.\build\debug\employee_directory_lesson_02.exe
```

## Expected Output

```
Objective: persist real employees and read them back after a restart.
Wrote 3 employees (Ana=18, Bruno=19, Carla=20)
After reopen, found "Ana" as employee 18 = Ana (12000)
```

The object ids (18/19/20) are deterministic for this exact sequence of
operations, but they're not "1, 2, 3" — the catalog itself consumes a few
ids first (type definitions, baselines) before your first real record.

## What to Notice

- Forgetting to re-`bind()` after `open()` is the most common mistake at
  this stage — every lesson in this course re-binds every type it
  touches, every single time it opens the file, for exactly this reason.
- The `shared_ptr`/registry attach isn't ceremony — it's why a `Handle`
  obtained in the first block would be meaningless in the second
  (different registry id, even same file).
- The name index exists purely as course infrastructure, introduced here
  because this is the first lesson that needs to find a record again
  after "restarting." Lesson 7 is where indexes and query planning are
  actually taught as a topic.

## Related Reference

- [docs/reference/database-lifecycle.md](../../../reference/database-lifecycle.md)
- [DEVELOPER_GUIDE.md, Chapter 3](../../../DEVELOPER_GUIDE.md#3-create-persist-reopen)
