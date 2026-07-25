# Lesson 19 -- Replication Catch-up and wal_only

> **Status:** code written; run after Lesson 18.

## What You'll Add

A replication operations briefing: inspect the primary's identity and storage
mode, name the files that `catch-up` and `seed-wal` need, and explain why a
`wal_only` primary changes the bootstrap path.

## New Concepts

- `replicate catch-up`, WAL manifests, spool directories, and persistent
  catch-up states.
- `PrimaryStorage::wal_only` and `CommitAckPolicy`.
- Why `bootstrap` copies data pages, while `seed-wal` reconstructs a follower
  from WAL.

## Starting Point

Lesson 13 bootstrapped a throwaway read replica. This lesson returns to the
same primary and covers the operational paths that keep or create followers
after the initial copy.

## Steps

- Open the primary and print its UUID, timeline, primary storage mode, and
  WAL path.
- Check whether a WAL file is currently present.
- Print the concrete CLI commands for `catch-up`, `status`, and `seed-wal`
  against the training paths.
- Explain the decision point: full primary can donate a snapshot; `wal_only`
  primary must use WAL seeding or another data replica.

## Full Listing (End of Lesson)

[lesson_19_replication_catchup_walonly.cpp](lesson_19_replication_catchup_walonly.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_19
.\build\debug\employee_directory_lesson_19.exe
```

## Expected Output

```
Objective: plan replication catch-up and wal_only operations.
Primary storage: full
Primary uuid: ...
Primary timeline: 1
WAL path: .../employee-directory.modb.wal
WAL present now: no
CLI catch-up shape:
  modb replicate catch-up ...
CLI wal_only seed shape:
  modb replicate seed-wal ...
```

## What to Notice

- `catch-up` is a resumable operational workflow, not just an apply loop.
- `wal_only` removes the source file that `bootstrap` would copy.
- The right replication command depends on what files the primary actually
  owns.

## Next

Continue with [Lesson 20 -- Performance and Hardening](../20-performance-and-hardening/20-performance-and-hardening.md).

## Related Reference

- [OPERACAO_REPLICACAO.md](../../../OPERACAO_REPLICACAO.md)
- [docs/reference/database-lifecycle.md](../../../reference/database-lifecycle.md)
- [DEVELOPER_GUIDE.md, Chapter 14](../../../DEVELOPER_GUIDE.md#14-background-read-replicas-and-wal_only)
