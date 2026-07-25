# Lesson 14 -- CLI and Diagnostics

> **Status:** code written; run after Lessons 1-13.

## What You'll Add

An operator's view of the employee directory: inspect the same database file
with the library calls behind `modb db check`, then connect that output to the
CLI commands you would use during backup, restore, and incident diagnosis.

## New Concepts

- `storage::check_database(path)` and the page-kind inventory behind
  `modb db check` -- see [OPERACAO.md](../../../OPERACAO.md).
- CLI command groups (`db`, `type`, `baseline`, `object`, `tx`, `replicate`)
  as operational tools, not application APIs -- see
  [USO_DA_CLI.md](../../../USO_DA_CLI.md).

## Starting Point

The persistent database file after Lessons 1-13 have run.

## Steps

- Open `employee-directory.modb`, print its current catalog baseline, and
  check whether a WAL file is present next to it.
- Run `storage::check_database(path)` and summarize the page inventory:
  slotted pages, blob pages, index pages, catalog pages, and unknown pages.
- Treat the result as an operator would: a green check means the page-level
  structure is internally consistent; it does not replace an application-level
  invariant check.
- Use the listed CLI commands as the next manual steps for a real incident:
  `db check`, `tx wal-info`, `baseline list`, and `replicate status`.

## Full Listing (End of Lesson)

[lesson_14_cli_diagnostics.cpp](lesson_14_cli_diagnostics.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_14
.\build\debug\employee_directory_lesson_14.exe
```

## Expected Output

```
Objective: inspect the training database the way an operator would.
Database file: .../docs/training/en/employee-directory.modb
Current baseline: ...
WAL present: no
Page inventory: total=...
  slotted=... blob=... index=... catalog=...
  unknown=0 errors=0
Database check: OK
Useful CLI follow-ups:
  modb db check ...
  modb tx wal-info ...
  modb baseline list ...
```

Exact page counts vary as earlier lessons evolve, but `unknown=0`,
`errors=0`, and `Database check: OK` are the important parts.

## What to Notice

- The CLI is not a separate product; it is a different surface over the same
  primitives the C++ application uses.
- `db check` answers "is the file structurally coherent?", not "is every HR
  rule satisfied?"
- The WAL file is part of the operational state. If it exists during backup,
  copy it with the database file.

## Next

Continue with [Lesson 15 -- Storage Internals](../15-storage-internals/15-storage-internals.md).

## Related Reference

- [OPERACAO.md](../../../OPERACAO.md)
- [USO_DA_CLI.md](../../../USO_DA_CLI.md)
- [FORMATO_DE_ARQUIVO.md](../../../FORMATO_DE_ARQUIVO.md)
