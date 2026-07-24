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
  [docs/reference/snapshots-mvcc.md](../../reference/snapshots-mvcc.md).
- Explicit garbage collection.

## Starting Point

Lesson 5's directory with departments, owned contacts, and project
assignments.

## Steps

- Implement the payroll report using `database->scan<Employee>(snapshot, visitor)`.
- Interleave the report with a concurrent raise (in the same process,
  sequenced to simulate "meanwhile") and show the report's total doesn't
  include the raise, while a fresh snapshot taken afterward does.
- Force a `snapshot_conflict` on purpose (hold a snapshot open, try to
  edit the same employee a second time) and handle it with a retry once
  the snapshot closes.
- Call `collect_garbage()` and print how many records it reclaimed.

## Full Listing (End of Lesson)

[examples/employee_directory/lesson_06_snapshots.cpp](../../../examples/employee_directory/lesson_06_snapshots.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_06
.\build\debug\employee_directory_lesson_06.exe
```

## Expected Output

```
Objective: a consistent payroll report, a snapshot_conflict, and manual GC.
Lesson 1: Employee type id = 16
Lesson 2: wrote 3 employees (Ana=18, Bruno=19, Carla=20)
Lesson 2: after reopen, employee 18 = Ana (12000)
Lesson 3: committed raise for Bruno
  Bruno after committed raise: Bruno = 10500
Lesson 3: uncommitted raise for Carla (deliberately not committed)
  Carla after scope exit (should be unchanged): Carla = 15000
Lesson 3: averaging Ana and Bruno's salaries in one transaction
  Ana after averaging: Ana = 11250
  Bruno after averaging: Bruno = 11250
Lesson 3: attempting a second transaction while one is open
  second begin() failed as expected: a transaction is already in progress
Lesson 4: Ana read through the new binding = Ana (11250, country=BR) -- country came from the declared default, not from disk
Lesson 4: raise for Ana via Handle::set (not manual materialize/update)
  Ana after Handle::set raise: Ana (13250, country=BR) -- now physically stored in the new 3-field shape
Lesson 5: created departments Engineering=31, Sales=32
Lesson 5: assigned Ana -> Engineering, Bruno -> Sales
Lesson 5: Diego=34 has emergency contact 33
Lesson 5: after removing Diego, his emergency contact is gone too (cascade-deleted): no object with id 33
Lesson 5: Carla's projects: Phoenix Atlas
Lesson 5: after removing Sales, resolving it directly fails as expected: no object with id 32 -- Bruno's `department` field still holds that (now dangling) id
Lesson 6: payroll total at the snapshot's epoch = 39500
Lesson 6: payroll total at the SAME snapshot after Carla's raise = 39500 (unchanged -- the snapshot doesn't see it)
Lesson 6: second raise while the snapshot is open failed as expected: object 20 already has a previous version visible to an older open snapshot
Lesson 6: retried raise for Carla succeeded once the snapshot closed
Lesson 6: payroll total on a fresh snapshot = 44500
Lesson 6: collect_garbage() reclaimed 12 record(s)
```

## What to Notice

- A snapshot is an epoch number, not a data copy — cheap to open, but
  every one you leave open blocks GC from reclaiming what it protects
  (the single retained `previous` version).
- `snapshot_conflict` is a signal to retry once the blocking snapshot
  closes, not a hard failure.

## Related Reference

- [docs/reference/snapshots-mvcc.md](../../reference/snapshots-mvcc.md)
- [GARANTIAS_TRANSACIONAIS.md](../../GARANTIAS_TRANSACIONAIS.md) §8-9
- [DEVELOPER_GUIDE.md, Chapter 7](../../DEVELOPER_GUIDE.md#7-snapshots-mvcc)
