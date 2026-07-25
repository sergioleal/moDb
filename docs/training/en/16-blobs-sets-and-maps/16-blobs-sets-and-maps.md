# Lesson 16 -- Blobs, Sets, and Maps

> **Status:** code written; run after Lesson 15.

## What You'll Add

A durable training artifact attached to the same employee-directory database:
a large note in `BlobStore`, a deduplicated `PersistentSet` of tags, and a
`PersistentMap` of report scores.

## New Concepts

- `BlobStore` as the storage layer under large binary values and collections.
- `PersistentSet<T>` and `PersistentMap<K,V>`, both backed by BLBP blob pages.
- Why collection writes require an active database transaction.

## Starting Point

The main `employee-directory.modb` file is still the chain artifact. Lesson 15
used a side lab file; this lesson returns to the main database and adds new
state that Lesson 17 will inspect.

## Steps

- Bind a new `TrainingArtifact` type with three `BlobId` fields.
- Create a note blob, a tag set, and a score map inside one transaction.
- Store the blob ids in a durable object named `ops-artifact`.
- Reopen the collection facades in the same run and verify deduplication and
  key lookup.

## Full Listing (End of Lesson)

[lesson_16_blobs_sets_maps.cpp](lesson_16_blobs_sets_maps.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_16
.\build\debug\employee_directory_lesson_16.exe
```

## Expected Output

```
Objective: add blob-backed training artifacts to the directory database.
Created TrainingArtifact ops-artifact as object ...
Note bytes: ...
Tags stored: 3 (after duplicate inserts)
Score for diagnostics: 95
```

## What to Notice

- Collections are not inline fields. The object stores only the `BlobId` of
  each collection.
- `PersistentSet` deduplicates by canonical element encoding.
- `PersistentMap::put` replaces an existing key instead of creating a second
  entry.

## Next

Continue with [Lesson 17 -- Catalog and Baselines](../17-catalog-and-baselines/17-catalog-and-baselines.md).

## Related Reference

- [docs/reference/relationships-collections.md](../../../reference/relationships-collections.md)
- [USO_DA_CLI.md](../../../USO_DA_CLI.md) -- `blob` and `coll`
