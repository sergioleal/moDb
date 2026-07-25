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
  [docs/reference/relationships-collections.md](../../../reference/relationships-collections.md).
- Persistent collections (`PersistentVector<Ref<T>>`) as a `BlobId` field,
  not a native member.

## Starting Point

The persistent database file after Lessons 1-4 have run, with `Employee`
in its `EmployeeV2` shape (name, salary, country).

## Steps

- Look Ana/Bruno/Carla up by name.
- Add `Department` (name) and bind it.
- Add `department: Ref<Department>` to `Employee`; create a couple of
  departments and assign employees to them. Also index `Department.name`
  (same reasoning as Lesson 2's `Employee.name` index) — Lesson 9 needs to
  find "Engineering" by name as a separate program run.
- Add `EmergencyContact` (name, phone) as an `OwnedRef` field on
  `Employee`; remove an employee and show the contact is cascade-deleted.
- Add `Project` (name) and a `projects: BlobId` field on `Employee` backed
  by `PersistentVector<Ref<Project>>`; assign an employee to two projects
  and list them back.
- Remove a `Department` that still has an employee pointing at it via
  `Ref`, and show the dangling reference fails with `record_not_found` on
  resolution rather than being silently prevented or cascaded.

## Full Listing (End of Lesson)

[lesson_05_relationships.cpp](lesson_05_relationships.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_05
.\build\debug\employee_directory_lesson_05.exe
```

## Expected Output

```
Objective: model departments, an owned emergency contact, and a project list.
Created departments Engineering=31, Sales=32
Assigned Ana -> Engineering, Bruno -> Sales
Diego=34 has emergency contact 33
After removing Diego, his emergency contact is gone too (cascade-deleted): no object with id 33
Carla's projects: Phoenix Atlas
After removing Sales, resolving it directly fails as expected: no object with id 32 -- Bruno's `department` field still holds that (now dangling) id
```

## What to Notice

- `Ref` and `OwnedRef` are wire-identical; only the cascade behavior
  differs, driven by which one you chose in the binding.
- After any collection mutation, the `BlobId` may change — the updated id
  must be re-stored on the owning `Employee` (see the `Handle::set` call
  right after `PersistentVector::push_back` in the code).

## Related Reference

- [docs/reference/relationships-collections.md](../../../reference/relationships-collections.md)
- [DEVELOPER_GUIDE.md, Chapter 6](../../../DEVELOPER_GUIDE.md#6-relationships-and-collections)
