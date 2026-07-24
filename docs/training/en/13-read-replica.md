# Lesson 13 (Optional, Advanced) — A Read Replica for Reporting

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

A second copy of the directory — a read-only follower — kept caught up from
the primary's WAL, so the payroll report from Lesson 6 can run against the
replica instead of competing with live writes on the primary.

## New Concepts

- Primary/follower model, `modb replicate bootstrap`/`apply-wal`/`status`,
  `set_read_only_replica` — see
  [OPERACAO_REPLICACAO.md](../../OPERACAO_REPLICACAO.md).
- *(Stretch goal)* `PrimaryStorage::wal_only` — a primary with no local data
  files at all, and `modb replicate seed-wal`.

## Starting Point

Lesson 6's payroll report (or any later lesson's directory — replication is
independent of the graph/facade/networking material).

## Steps

- TODO: bootstrap a follower file from the primary directory via the CLI
  (`modb replicate bootstrap`) or the equivalent library calls.
- TODO: apply a batch of WAL records to the follower
  (`modb replicate apply-wal`) after making changes on the primary.
- TODO: open the follower with `set_read_only_replica(true)` and run the
  Lesson 6 payroll report against it instead of the primary.
- TODO: confirm the follower rejects writes (`begin()` fails with
  `replica_read_only`).
- TODO: *(stretch)* recreate the primary with
  `DatabaseOptions{.primary_storage = PrimaryStorage::wal_only}` and seed the
  follower with `modb replicate seed-wal` instead of a file-copy bootstrap.

## Full Listing (End of Lesson)

TODO — target: `examples/employee_directory/lesson_13_replica.cpp`, plus a
short CLI transcript for the bootstrap/apply-wal steps.

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_13
.\build\debug\employee_directory_lesson_13.exe
```

## Expected Output

TODO — the payroll report producing the same total when run against the
follower, and a rejected write attempt on the follower.

## What to Notice

- TODO: primary and follower must never share a data volume.
- TODO: a `wal_only` primary can't donate a file-copy bootstrap
  (`data_files_disabled`) — that's what `seed-wal` is for.

## Related Reference

- [OPERACAO_REPLICACAO.md](../../OPERACAO_REPLICACAO.md)
- [docs/reference/database-lifecycle.md](../../reference/database-lifecycle.md)
- [DEVELOPER_GUIDE.md, Chapter 14](../../DEVELOPER_GUIDE.md#14-background-read-replicas-and-wal_only)
