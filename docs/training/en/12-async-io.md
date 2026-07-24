# Lesson 12 (Optional, Advanced) — Asynchronous WAL I/O

> **Status:** ✅ code written and verified against a real build.

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

- Reopen the same employee-directory file twice: once with default
  `DatabaseOptions{}` (`wal_io = sync`), once with
  `DatabaseOptions{.wal_io = WalIoMode::async}`. `wal_io` is a per-open
  runtime choice, not a persisted format decision, so the same file works
  under either mode.
- Run 200 committed raises against Carla under each mode, timing the
  whole loop with `std::chrono::steady_clock`.
- Print both timings and commits/second, and report honestly whichever
  mode came out ahead on this run — don't assume `async` wins.
- Settle Carla's salary back to a clean value (20000) at the end so later
  lessons aren't left with whatever value the last timing iteration
  happened to write.

## Full Listing (End of Lesson)

[examples/employee_directory/lesson_12_async_io.cpp](../../../examples/employee_directory/lesson_12_async_io.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_12
.\build\debug\employee_directory_lesson_12.exe
```

## Expected Output

The prose lines match exactly every run; the millisecond/commits-per-second
numbers will vary by machine and disk — treat the ones below as one real
sample, not a target to hit:

```
Objective: compare sync vs. async WAL I/O for the same workload.
...
Lesson 12: 200 committed raises under wal_io=sync took 907.03 ms (220.50 commits/s)
Lesson 12: 200 committed raises under wal_io=async took 946.40 ms (211.33 commits/s)
Lesson 12: sync was faster (or tied) on this run (1.04x)
Lesson 12: settled Carla's salary back to 20000 for later lessons
```

(the `...` stands in for Lessons 1-11's output, unchanged from
[Lesson 11](11-graphs.md#expected-output))

## What to Notice

- This lesson is deliberately about *not* reaching for an option just
  because it exists — on the run captured above, `sync` was marginally
  faster than `async`, consistent with
  [OPERACAO_IO_ASSINCRONO.md](../../OPERACAO_IO_ASSINCRONO.md)'s "no
  consistent win yet" finding. Measure your own workload before opting in.

## Related Reference

- [OPERACAO_IO_ASSINCRONO.md](../../OPERACAO_IO_ASSINCRONO.md)
- [DEVELOPER_GUIDE.md, Chapter 13](../../DEVELOPER_GUIDE.md#13-optional-asynchronous-wal-io)
