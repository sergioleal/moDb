# Lesson 6 — Consistent Reports with Snapshots

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

A "total payroll by department" report that takes a `Snapshot` before
running, so it reflects one consistent instant even if raises are being
applied to other employees while the report iterates. You'll also add a
manual "compact the directory" maintenance command wired to
`collect_garbage()`.

## New Concepts

- `Database::snapshot()`, epoch-based reads (`get`/`scan` with a
  `Snapshot`), single retained `previous` version, `snapshot_conflict` — see
  [docs/reference/snapshots-mvcc.md](../../reference/snapshots-mvcc.md).
- Explicit garbage collection.

## Starting Point

Lesson 5's directory with departments, owned contacts, and project
assignments.

## Steps

- TODO: implement the payroll report using `database->scan<Employee>(snapshot, visitor)`.
- TODO: interleave the report with a concurrent raise (in the same process,
  sequenced to simulate "meanwhile") and show the report's total doesn't
  include the raise, while a fresh (non-snapshotted) read does.
- TODO: force a `snapshot_conflict` on purpose (hold a snapshot open, edit
  the same employee twice) and handle it with a retry.
- TODO: add a `compact` command calling `collect_garbage()` and print how
  many records it reclaimed.

## Full Listing (End of Lesson)

TODO — target: `examples/employee_directory/lesson_06_snapshots.cpp`.

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_06
.\build\debug\employee_directory_lesson_06.exe
```

## Expected Output

TODO — a report total that stays stable across a concurrent raise, a
`snapshot_conflict` demonstrated and retried, and a nonzero reclaim count
from `compact`.

## What to Notice

- TODO: a snapshot is an epoch number, not a data copy — cheap to open,
  but every one you leave open blocks GC from reclaiming what it protects.
- TODO: `snapshot_conflict` is a signal to retry, not a hard failure.

## Related Reference

- [docs/reference/snapshots-mvcc.md](../../reference/snapshots-mvcc.md)
- [GARANTIAS_TRANSACIONAIS.md](../../GARANTIAS_TRANSACIONAIS.md) §8-9
- [DEVELOPER_GUIDE.md, Chapter 7](../../DEVELOPER_GUIDE.md#7-snapshots-mvcc)
