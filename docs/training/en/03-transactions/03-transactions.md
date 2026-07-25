# Lesson 3 — Transactions

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

A "give a raise" operation that does it properly: one transaction, an
explicit commit, and a deliberate demonstration of what happens when you
*don't* commit (rollback-on-scope-exit). You'll also add a small
"transfer department budget" style operation touching two records in one
transaction, to show atomicity actually matters.

## New Concepts

- `begin()`/`commit()`/`rollback()`, single-writer, RAII
  rollback-on-scope-exit, the WAL-before-pages commit protocol — see
  [GARANTIAS_TRANSACIONAIS.md](../../../GARANTIAS_TRANSACIONAIS.md) §1-2/4/6.

## Starting Point

The persistent database file after Lessons 1-2 have run — Ana, Bruno, and
Carla already exist, findable by name through Lesson 2's index.

## Steps

- Open the file, bind, and look up Ana/Bruno/Carla's ids by name (the
  same `find_employee_id` helper Lesson 2 introduced — every lesson from
  here on starts this way).
- Add a function that raises one employee's salary inside a transaction,
  commits, and confirms the change stuck.
- Deliberately write a second version that begins a transaction, makes a
  change, and lets the `Transaction` go out of scope without calling
  `commit()` — show the change did *not* survive.
- Touch two employees in one transaction (average their salaries) to
  demonstrate atomicity of multi-object changes.
- Try opening a second transaction while one is active and show the
  `transaction_active` failure.

## Full Listing (End of Lesson)

[lesson_03_transactions.cpp](lesson_03_transactions.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_03
.\build\debug\employee_directory_lesson_03.exe
```

## Expected Output

```
Objective: commit a raise properly, show an uncommitted one rolling back, and prove single-writer.
Committed raise for Bruno: salary = 10500
Uncommitted raise for Carla (deliberately not committed): salary is still 15000
Averaged Ana and Bruno's salaries in one transaction: Ana = 11250, Bruno = 11250
A second begin() while one is already open failed as expected: a transaction is already in progress
```

## What to Notice

- Forgetting `commit()` isn't a bug you need to guard against
  defensively — it's the safety net working as intended.
- Single-writer means concurrent readers are fine, but this app can't
  have two threads both mid-transaction at once.

## Related Reference

- [GARANTIAS_TRANSACIONAIS.md](../../../GARANTIAS_TRANSACIONAIS.md)
- [DEVELOPER_GUIDE.md, Chapter 4](../../../DEVELOPER_GUIDE.md#4-transactions)
