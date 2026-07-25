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
  [FACADES.md](../../../FACADES.md).

## Starting Point

The persistent database file after Lessons 1-9 have run.

## Steps

- Look Bruno, Ana, and "Engineering" up by name.
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

[lesson_10_facades.cpp](lesson_10_facades.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_10
.\build\debug\employee_directory_lesson_10.exe
```

## Expected Output

```
Objective: expose a stable "hr" facade instead of raw operation ids.
Opening the "hr" facade (version 1)
Moving Bruno back to Engineering through HRFacade::TransferDepartment
  succeeded
Giving Ana a raise through HRFacade::GiveRaise
  succeeded
A client compiled against "hr" version 2 asks for a version the server never published
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

- [FACADES.md](../../../FACADES.md)
- [docs/reference/domain-operations.md](../../../reference/domain-operations.md)
- [DEVELOPER_GUIDE.md, Chapter 11](../../../DEVELOPER_GUIDE.md#11-facades-versioned-remote-surfaces)
