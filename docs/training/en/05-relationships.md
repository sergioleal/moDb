# Lesson 5 — Relationships: Departments and Projects

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

The directory grows a `Department` type and links each `Employee` to one
via `Ref<Department>`. You'll also add an owned sub-record (e.g. an
`EmergencyContact` via `OwnedRef`, deleted automatically with the employee)
and a `PersistentVector<Ref<Project>>` so an employee can be assigned to
several projects.

## New Concepts

- `Ref<T>` (association) vs `OwnedRef<T>` (composition/cascade-delete) vs
  `Embedded<T>` — see
  [docs/reference/relationships-collections.md](../../reference/relationships-collections.md).
- Persistent collections (`PersistentVector<Ref<T>>`) as a `BlobId` field,
  not a native member.

## Starting Point

Lesson 4's directory with `EmployeeV2` (name, salary, country).

## Steps

- Add `Department` (name) and bind it.
- Add `department: Ref<Department>` to `Employee`; create a couple of
  departments and assign employees to them.
- Add `EmergencyContact` (name, phone) as an `OwnedRef` field on
  `Employee`; remove an employee and show the contact is cascade-deleted.
- Add `Project` (name) and a `projects: BlobId` field on `Employee` backed
  by `PersistentVector<Ref<Project>>`; assign an employee to two projects
  and list them back.
- Remove a `Department` that still has an employee pointing at it via
  `Ref`, and show the dangling reference fails with `record_not_found` on
  resolution rather than being silently prevented or cascaded.

## Full Listing (End of Lesson)

[examples/employee_directory/lesson_05_relationships.cpp](../../../examples/employee_directory/lesson_05_relationships.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_05
.\build\debug\employee_directory_lesson_05.exe
```

## Expected Output

```
Objective: model departments, an owned emergency contact, and a project list.
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
```

## What to Notice

- `Ref` and `OwnedRef` are wire-identical; only the cascade behavior
  differs, driven by which one you chose in the binding.
- After any collection mutation, the `BlobId` may change — the updated id
  must be re-stored on the owning `Employee` (see the `Handle::set` call
  right after `PersistentVector::push_back` in the code).

## Related Reference

- [docs/reference/relationships-collections.md](../../reference/relationships-collections.md)
- [DEVELOPER_GUIDE.md, Chapter 6](../../DEVELOPER_GUIDE.md#6-relationships-and-collections)
