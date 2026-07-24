# Lesson 10 — Facades: A Stable HR API

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

An `HRFacade` grouping `TransferDepartment` (from Lesson 9) and a new
`GiveRaise` operation behind one versioned, typed surface. The client opens
the facade once and calls both methods through it, without knowing the
server's concrete operation classes.

## New Concepts

- `FacadeHandle<TFacade>::invoke<Method>`, `FacadeCatalog`, id+version
  identity (not array index), embedded vs. remote invoker — see
  [FACADES.md](../../FACADES.md).

## Starting Point

Lesson 9's server with `TransferDepartment` registered, still one process.

## Steps

- Add a `GiveRaise` operation (`k_id = "employee.give_raise"`, args
  `employee_id` + `new_salary`), same shape as `TransferDepartment`.
- Define the `HRFacade` tag (`k_id = "hr"`, `k_version = 1`) and its
  `FacadeDescriptor` listing both `TransferDepartment` and `GiveRaise` as
  `MethodDescriptor`s.
- Add `.facades = {hr_facade_descriptor()}` to the `ModuleManifest`, and
  load it with the `ModuleLoader::load` overload that also takes a
  `FacadeCatalog` (register both an `OperationRegistry` *and* a
  `FacadeCatalog` on the server).
- Client: `connection->open_facade<HRFacade>()`, then
  `handle->invoke<TransferDepartment>(...)` and
  `handle->invoke<GiveRaise>(...)` — both go through the same typed
  handle.
- Demonstrate a version mismatch: define a second tag `HRFacadeV2` with
  `k_version = 2` (a version the server never registered) and show
  `connection->open_facade<HRFacadeV2>()` fails.

## Full Listing (End of Lesson)

[examples/employee_directory/lesson_10_facades.cpp](../../../examples/employee_directory/lesson_10_facades.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_10
.\build\debug\employee_directory_lesson_10.exe
```

## Expected Output

```
Objective: expose a stable "hr" facade instead of raw operation ids.
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
  employees earning >= 15000 (no index yet): access=table_scan index_available=false
  results: Carla
Lesson 7: created an index on Employee.salary
  employees earning >= 15000 (indexed): access=index_scan index_available=true
  results: Carla
Lesson 7: top 2 earners: Carla(20000) Ana(13250)
Lesson 8: handshake ok, protocol 1.0, max_concurrent_streams=4
Lesson 8: remote query returned 3 employees
Lesson 8: remote search for salary == 20000 returned 1 match(es): Carla
Lesson 9: transferring Bruno to Engineering (his old department, Sales, was removed back in Lesson 5)
  succeeded
Lesson 9: attempting a transfer to a department that doesn't exist
  failed as expected: target department does not exist
Lesson 10: opening the "hr" facade (version 1)
Lesson 10: moving Bruno back to Engineering through HRFacade::TransferDepartment
  succeeded
Lesson 10: giving Ana a raise through HRFacade::GiveRaise
  succeeded
Lesson 10: a client compiled against "hr" version 2 asks for a version the server never published
  rejected as expected: incompatible facade version for 'hr': requested 2
```

## What to Notice

- A facade is a naming/versioning layer, not a second execution engine —
  `TransferDepartment` and `GiveRaise` still run exactly as they did in
  Lesson 9; the facade only adds discoverability and a version check in
  front of `invoke<Method>`.
- Identity is `FacadeId + version`, never the position in a list —
  `HRFacadeV2` fails not because "hr" doesn't exist, but because no
  version 2 of "hr" was ever registered.

## Related Reference

- [FACADES.md](../../FACADES.md)
- [docs/reference/domain-operations.md](../../reference/domain-operations.md)
- [DEVELOPER_GUIDE.md, Chapter 11](../../DEVELOPER_GUIDE.md#11-facades-versioned-remote-surfaces)
