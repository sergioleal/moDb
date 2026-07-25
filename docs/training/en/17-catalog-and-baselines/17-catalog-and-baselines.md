# Lesson 17 -- Catalog and Baselines

> **Status:** code written; run after Lesson 16.

## What You'll Add

A schema audit for the `TrainingArtifact` object from Lesson 16. You will add
one field, watch Ring0 create a new baseline, then prove the older baseline is
still loadable by id.

## New Concepts

- Baselines as immutable catalog snapshots.
- Additive schema evolution and default values.
- Why module manifests and facades name a baseline instead of trusting "the
  latest schema".

## Starting Point

Lesson 16 created `TrainingArtifact{name,note,tags,scores}` and indexed it by
name. This lesson binds `TrainingArtifact` again with one extra field.

## Steps

- Open the database and record the current baseline id.
- Bind `TrainingArtifactV2`, adding `owner` as field 5 with default
  `"platform-team"`.
- Confirm the current baseline id changed and the previous baseline can still
  be found.
- Materialize `ops-artifact` through the new binding and observe the defaulted
  owner.

## Full Listing (End of Lesson)

[lesson_17_catalog_baselines.cpp](lesson_17_catalog_baselines.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_17
.\build\debug\employee_directory_lesson_17.exe
```

## Expected Output

```
Objective: evolve a training artifact and inspect catalog baselines.
Baseline before TrainingArtifactV2: ...
Baseline after TrainingArtifactV2: ...
Historical baseline still loadable: yes
ops-artifact owner through V2 binding: platform-team
```

## What to Notice

- A baseline is a named snapshot of active type definitions.
- Additive schema evolution creates a new active baseline without deleting the
  old one.
- Defaults make old bytes readable through the new shape, but that does not
  mean the old bytes already contain the new field.

## Next

Continue with [Lesson 18 -- Protocol and Compatibility](../18-protocol-and-compatibility/18-protocol-and-compatibility.md).

## Related Reference

- [docs/reference/object-model.md](../../../reference/object-model.md)
- [FACADES.md](../../../FACADES.md)
- [COMPATIBILIDADE.md](../../../COMPATIBILIDADE.md)
