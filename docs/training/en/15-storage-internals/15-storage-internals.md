# Lesson 15 -- Storage Internals

> **Status:** code written; run after Lesson 14.

## What You'll Add

A small physical-storage lab next to the employee directory. You will create a
throwaway database file, allocate a raw page, format it as a `SlottedPage`, and
observe why higher-level objects eventually become page and slot operations.

## New Concepts

- `PageFile`, fixed-size pages, page zero, and explicit flush.
- `SlottedPage`, variable-size records, slot ids, generations, free space, and
  lazy compaction -- see [FORMATO_DE_ARQUIVO.md](../../../FORMATO_DE_ARQUIVO.md).

## Starting Point

Lesson 14 has shown the inventory of the real employee database. This lesson
zooms into one of those physical page types in a separate lab file.

## Steps

- Create `employee-directory-storage-lab.modb` next to the course database.
- Allocate one page and format it as a `SlottedPage`.
- Insert two records, update one, erase the other, and persist the page through
  `PageFile::write()` and `flush()`.
- Reopen the file through `check_database()` and confirm the lab file is still
  structurally valid.

## Full Listing (End of Lesson)

[lesson_15_storage_internals.cpp](lesson_15_storage_internals.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_15
.\build\debug\employee_directory_lesson_15.exe
```

## Expected Output

```
Objective: inspect the page and slot layer under object storage.
Created storage lab: .../employee-directory-storage-lab.modb
Allocated record page: 1
Inserted two records: slots 0 and 1
Updated slot 0 and erased slot 1
Free space after edits: ...
Database check for the lab file: OK
```

## What to Notice

- A stable logical object id is not the same thing as a physical page/slot.
- The slot directory is what lets a variable-size record move inside the page
  without changing the slot id.
- This file is a lab artifact. The main lesson chain still continues through
  `employee-directory.modb`.

## Next

Continue with [Lesson 16 -- Blobs, Sets, and Maps](../16-blobs-sets-and-maps/16-blobs-sets-and-maps.md).

## Related Reference

- [FORMATO_DE_ARQUIVO.md](../../../FORMATO_DE_ARQUIVO.md)
- [USO_DA_CLI.md](../../../USO_DA_CLI.md) -- `page`, `record`, `heap`
