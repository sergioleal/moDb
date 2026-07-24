# Lesson 12 (Optional, Advanced) — Asynchronous WAL I/O

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

Nothing user-facing — this lesson is about **measuring** an internal
option: reopening the directory with `DatabaseOptions::wal_io = WalIoMode::async`
and comparing commit throughput for a batch of employee updates against the
default synchronous WAL.

## New Concepts

- `DatabaseOptions::wal_io`, `AsyncWalSink`, why this is opt-in and
  currently unproven — see
  [OPERACAO_IO_ASSINCRONO.md](../../OPERACAO_IO_ASSINCRONO.md).

## Starting Point

Any earlier lesson's directory — this one is a side-quest, not a required
dependency for Lesson 13.

## Steps

- TODO: reopen the same directory twice, once with default
  `DatabaseOptions` (`wal_io = sync`) and once with `wal_io = async`.
- TODO: run the same batch of N transactions (e.g. N raises) against each
  and time them.
- TODO: compare against the measured numbers already published in
  [OPERACAO_IO_ASSINCRONO.md](../../OPERACAO_IO_ASSINCRONO.md) — the point
  of this lesson is to reproduce that "no consistent win yet" finding
  yourself, not to assume `async` helps.

## Full Listing (End of Lesson)

TODO — target: `examples/employee_directory/lesson_12_async_io.cpp`.

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_12
.\build\debug\employee_directory_lesson_12.exe
```

## Expected Output

TODO — two timings printed side by side, sync vs. async.

## What to Notice

- TODO: this lesson is deliberately about *not* reaching for an option just
  because it exists — measure your own workload before opting in.

## Related Reference

- [OPERACAO_IO_ASSINCRONO.md](../../OPERACAO_IO_ASSINCRONO.md)
- [DEVELOPER_GUIDE.md, Chapter 13](../../DEVELOPER_GUIDE.md#13-optional-asynchronous-wal-io)
