# Lesson 3 — Transactions

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

A "give a raise" operation that does it properly: one transaction, an
explicit commit, and a deliberate demonstration of what happens when you
*don't* commit (rollback-on-scope-exit). You'll also add a small
"transfer department budget" style operation touching two records in one
transaction, to show atomicity actually matters.

## New Concepts

- `begin()`/`commit()`/`rollback()`, single-writer, RAII
  rollback-on-scope-exit, the WAL-before-pages commit protocol — see
  [GARANTIAS_TRANSACIONAIS.md](../../GARANTIAS_TRANSACIONAIS.md) §1-2/4/6.

## Starting Point

Lesson 2's directory with a handful of persisted employees.

## Steps

- TODO: add a function that raises one employee's salary inside a
  transaction, commits, and confirms the change stuck.
- TODO: deliberately write a second version that begins a transaction,
  makes a change, and lets the `Transaction` go out of scope without
  calling `commit()` — show the change did *not* survive.
- TODO: touch two employees in one transaction (e.g. "swap" or "average"
  their salaries) to demonstrate atomicity of multi-object changes.
- TODO: try opening a second transaction while one is active and show the
  `transaction_active` failure.

## Full Listing (End of Lesson)

TODO — target: `examples/employee_directory/lesson_03_transactions.cpp`.

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_03
.\build\debug\employee_directory_lesson_03.exe
```

## Expected Output

TODO — should show: a raise that stuck, a raise that didn't (uncommitted),
and a rejected second `begin()`.

## What to Notice

- TODO: forgetting `commit()` isn't a bug you need to guard against
  defensively — it's the safety net working as intended.
- TODO: single-writer means concurrent readers are fine, but this app
  can't have two threads both mid-transaction at once.

## Related Reference

- [GARANTIAS_TRANSACIONAIS.md](../../GARANTIAS_TRANSACIONAIS.md)
- [DEVELOPER_GUIDE.md, Chapter 4](../../DEVELOPER_GUIDE.md#4-transactions)
