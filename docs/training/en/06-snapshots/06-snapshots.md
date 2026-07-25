# Lesson 6 — Consistent Reports with Snapshots

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

A "total payroll by department" report that takes a `Snapshot` before
running, so it reflects one consistent instant even if raises are being
applied to other employees while the report iterates. You'll also add a
manual "compact the directory" maintenance command wired to
`collect_garbage()`.

## New Concepts

- `Database::snapshot()`, epoch-based reads (`get`/`scan` with a
  `Snapshot`), single retained `previous` version, `snapshot_conflict` — see
  [docs/reference/snapshots-mvcc.md](../../../reference/snapshots-mvcc.md).
- Explicit garbage collection.

## Starting Point

The persistent database file after Lessons 1-5 have run, with
departments, an owned contact, and project assignments already in place.

## Steps

- Look Carla up by name.
- Implement the payroll report using `database->scan<Employee>(snapshot, visitor)`.
- Interleave the report with a concurrent raise (in the same process,
  sequenced to simulate "meanwhile") and show the report's total doesn't
  include the raise, while a fresh snapshot taken afterward does.
- Force a `snapshot_conflict` on purpose (hold a snapshot open, try to
  edit the same employee a second time) and handle it with a retry once
  the snapshot closes.
- Call `collect_garbage()` and print how many records it reclaimed.

## Full Listing (End of Lesson)

[lesson_06_snapshots.cpp](lesson_06_snapshots.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_06
.\build\debug\employee_directory_lesson_06.exe
```

## Expected Output

```
Objective: a consistent payroll report, a snapshot_conflict, and manual GC.
Payroll total at the snapshot's epoch = 39500
Payroll total at the SAME snapshot after Carla's raise = 39500 (unchanged -- the snapshot doesn't see it)
A second raise while the snapshot is open failed as expected: object 20 already has a previous version visible to an older open snapshot
Retried raise for Carla succeeded once the snapshot closed
Payroll total on a fresh snapshot = 44500
collect_garbage() reclaimed 12 record(s)
```

## What to Notice

- A snapshot is an epoch number, not a data copy — cheap to open, but
  every one you leave open blocks GC from reclaiming what it protects
  (the single retained `previous` version).
- `snapshot_conflict` is a signal to retry once the blocking snapshot
  closes, not a hard failure.

## Related Reference

- [docs/reference/snapshots-mvcc.md](../../../reference/snapshots-mvcc.md)
- [GARANTIAS_TRANSACIONAIS.md](../../../GARANTIAS_TRANSACIONAIS.md) §8-9
- [DEVELOPER_GUIDE.md, Chapter 7](../../../DEVELOPER_GUIDE.md#7-snapshots-mvcc)
