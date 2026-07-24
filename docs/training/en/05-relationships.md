# Lesson 5 — Relationships: Departments and Projects

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

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

- TODO: add `Department` (name) and bind it.
- TODO: add `department: Ref<Department>` to `Employee`; create a couple of
  departments and assign employees to them.
- TODO: add `EmergencyContact` (name, phone) as an `OwnedRef` field on
  `Employee`; remove an employee and show the contact is cascade-deleted.
- TODO: add `Project` (name) and a `projects: BlobId` field on `Employee`
  backed by `PersistentVector<Ref<Project>>`; assign an employee to two
  projects and list them back.
- TODO: remove a `Department` that still has employees pointing at it via
  `Ref`, and show the dangling reference fails with `record_not_found` on
  resolution rather than being silently prevented or cascaded.

## Full Listing (End of Lesson)

TODO — target: `examples/employee_directory/lesson_05_relationships.cpp`.

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_05
.\build\debug\employee_directory_lesson_05.exe
```

## Expected Output

TODO — an employee with a resolved department, a cascade-deleted emergency
contact, a listed project assignment, and a demonstrated dangling `Ref`.

## What to Notice

- TODO: `Ref` and `OwnedRef` are wire-identical; only the cascade behavior
  differs, driven by which one you chose in the binding.
- TODO: after any collection mutation, the `BlobId` may change — the
  updated id must be re-stored on the owning `Employee`.

## Related Reference

- [docs/reference/relationships-collections.md](../../reference/relationships-collections.md)
- [DEVELOPER_GUIDE.md, Chapter 6](../../DEVELOPER_GUIDE.md#6-relationships-and-collections)
