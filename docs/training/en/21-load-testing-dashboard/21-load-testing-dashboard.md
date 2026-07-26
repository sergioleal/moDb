# Lesson 21 -- Load Testing and Dashboard

> **Status:** code written; optional operational lesson.

## What You'll Add

A hands-on map for the load-test tool: list the available cases, run a small
campaign, index the generated JSONL into history, and open the dashboard that
turns those results into a trend you can inspect.

## New Concepts

- `modb_load` is the load-test CLI, separate from `modb_bench`.
- A load run writes raw JSONL records into `load-results/`.
- `modb_load index` folds raw run files into `load-history/series.jsonl`.
- The dashboard reads the indexed history file; it does not run the tests.

## Starting Point

Lesson 20 connected the course to benchmarks, load tests, and fuzzing. This
lesson zooms in on the load-test side and shows the concrete operator workflow:
plan, run, index, inspect.

## Steps

- Build `modb_load` and this lesson.
- Use `list-profiles` and `list-cases` to inspect what would run.
- Run `load-smoke` or the wrapper script with a tiny, known-good config.
- Read the path printed by `modb_load run`; that file is the raw campaign JSONL.
- Let automatic indexing update `load-history/series.jsonl`, or run
  `modb_load index` explicitly.
- Open `loadtests/dashboard/index.html` and point it at the history file.

## Full Listing (End of Lesson)

[lesson_21_load_testing_dashboard.cpp](lesson_21_load_testing_dashboard.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target modb_load employee_directory_lesson_21
.\build\debug\employee_directory_lesson_21.exe
```

## Expected Output

```
Objective: use modb_load and inspect results in the dashboard.
Found loadtests/config/load-smoke.yaml
Found loadtests/dashboard/index.html
Found loadtests/environments.json
Tool target to build: modb_load
Suggested workflow:
  modb_load list-profiles
  modb_load list-cases --profile load-smoke --workload create_only --target embedded
  modb_load run --profile load-smoke --workload create_only --target embedded --scale 1k --accept-unknown-budget
  modb_load index <result.jsonl> --history-file load-history/series.jsonl
  open loadtests/dashboard/index.html and load load-history/series.jsonl
```

## Try It

From the repository root:

```powershell
cmake --build --preset debug --target modb_load
.\build\debug\modb_load.exe list-profiles
.\build\debug\modb_load.exe list-cases --profile load-smoke --workload create_only --target embedded
.\build\debug\modb_load.exe run --profile load-smoke --workload create_only --target embedded --scale 1k --accept-unknown-budget
```

The `run` command prints a result path such as:

```text
Resultado: load-results/modb-load-....jsonl  run_id=...  status=completed
```

If automatic indexing is enabled, `load-history/series.jsonl` is updated at the
end of the run. If you have a raw file from another machine, index it manually:

```powershell
.\build\debug\modb_load.exe index load-results\<result-file>.jsonl --history-file load-history\series.jsonl
```

Then open:

```text
loadtests/dashboard/index.html
```

Use the dashboard's file picker to load `load-history/series.jsonl`.

## What to Notice

- `list-cases` is the cheap safety check before a run: it resolves profiles,
  filters, scales, targets, and estimated budget without mutating data.
- Raw campaign files and indexed history serve different jobs. Keep raw JSONL
  for audit; use `series.jsonl` for trend and dashboard views.
- Embedded and loopback are both load-test targets, but not every workload has
  dispatch for every target yet. If a case is not implemented, the tool should
  say so clearly.

## Next

This is the last lesson in the current training track. From here, use the
dashboard and `modb_load gate` to compare future changes against the history you
collect.

## Related Reference

- [PLANO_TESTES_DE_CARGA.md](../../../PLANO_TESTES_DE_CARGA.md)
- [PLANO_IMPLEMENTACAO_CARGA.md](../../../../docs-process/PLANO_IMPLEMENTACAO_CARGA.md)
- [loadtests/dashboard/index.html](../../../../loadtests/dashboard/index.html)
