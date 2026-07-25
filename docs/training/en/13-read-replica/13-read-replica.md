# Lesson 13 (Optional, Advanced) — A Read Replica for Reporting

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

A second copy of the directory — a read-only follower — kept caught up from
the primary's WAL, so the payroll report from Lesson 6 can run against the
replica instead of competing with live writes on the primary.

## New Concepts

- Primary/follower model, `modb replicate bootstrap`/`apply-wal`/`status`,
  `set_read_only_replica` — see
  [OPERACAO_REPLICACAO.md](../../../OPERACAO_REPLICACAO.md).
- *(Stretch goal)* `PrimaryStorage::wal_only` — a primary with no local data
  files at all, and `modb replicate seed-wal`.

## Starting Point

Lesson 12's directory (the graph/facade/networking material from Lessons
8-11 stays in place; replication doesn't touch any of it).

## Steps

- Bootstrap a follower using the library calls directly (the same calls
  behind `modb replicate bootstrap`): `repl::create_bootstrap_snapshot(
  primary, temp_dir)` copies the primary's data file under a brief writer
  barrier; `repl::install_bootstrap_snapshot(snapshot, follower_path)`
  installs it at a new path next to the main directory file.
- Open the follower file, re-`bind()` every type, confirm its
  `database_uuid()` matches the primary's, and call
  `set_read_only_replica(true)`.
- Run a Lesson-6-style payroll report (`scan<Employee>(snapshot,
  visitor)`) against the follower's own snapshot.
- Confirm the follower rejects writes: `follower->begin()` fails with
  `ErrorCode::replica_read_only`.

This lesson does **not** implement the `apply-wal`/streaming half of
replication (keeping a follower continuously caught up) or the
`wal_only` + `seed-wal` stretch goal — the bootstrap snapshot above is a
one-time copy, current as of its `cut_lsn`. See
[OPERACAO_REPLICACAO.md](../../../OPERACAO_REPLICACAO.md) for how streaming
apply and `wal_only` primaries extend this.

## Full Listing (End of Lesson)

[lesson_13_read_replica.cpp](lesson_13_read_replica.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_13
.\build\debug\employee_directory_lesson_13.exe
```

## Expected Output

```
Objective: bootstrap a read-only replica and query it independently.
...
Lesson 13: bootstrap snapshot cut at LSN 3499 (90112 bytes)
Lesson 13: installed the follower copy at <temp-dir>\employee-directory-<timestamp>-replica.modb
Lesson 13: follower's database_uuid matches the primary's
Lesson 13: payroll total from the follower = 54750
Lesson 13: follower rejected a write attempt as expected: follower rejects begin/write
```

(the `...` stands in for Lessons 1-12's output, unchanged from
[Lesson 12](../12-async-io/12-async-io.md#expected-output); the `cut_lsn`, byte count, and
payroll total will match this exactly for an unmodified run of the
lesson, since every step up to this point is deterministic)

## What to Notice

- Primary and follower must never share a data volume — the follower here
  is deliberately a separate file (`*-replica.modb`), not another handle
  onto the same path.
- `create_bootstrap_snapshot` briefly opens and rolls back a transaction
  on the primary purely as a writer barrier — it's not a lasting lock, but
  it does mean bootstrap can't run concurrently with another open write.
- A `wal_only` primary can't donate a file-copy bootstrap at all
  (`data_files_disabled` — see `create_bootstrap_snapshot`'s first check);
  that's what `seed-wal` is for, and it's out of scope for this lesson.

## Related Reference

- [OPERACAO_REPLICACAO.md](../../../OPERACAO_REPLICACAO.md)
- [docs/reference/database-lifecycle.md](../../../reference/database-lifecycle.md)
- [DEVELOPER_GUIDE.md, Chapter 14](../../../DEVELOPER_GUIDE.md#14-background-read-replicas-and-wal_only)
