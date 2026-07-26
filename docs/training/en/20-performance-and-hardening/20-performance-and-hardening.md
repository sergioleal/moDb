# Lesson 20 -- Performance and Hardening

> **Status:** code written; final lesson in this track.

## What You'll Add

A closing production-readiness checklist: locate the benchmark plan, load-test
plan, fuzzing corpus, and scripts that turn the employee-directory lessons into
measurable, repeatable engineering work.

## New Concepts

- Benchmarks answer "how fast is this operation under controlled conditions?"
- Load tests answer "does the system stay correct and predictable at volume?"
- Fuzzing hardens byte decoders by replaying hostile or truncated inputs.

## Starting Point

Lessons 1-19 taught the application and operational surfaces. This lesson ties
them to the evidence you need before changing performance-sensitive or
untrusted-input code.

## Steps

- Check that the benchmark, load-test, and fuzzing documents exist.
- Check that the runner scripts and fuzz corpora exist.
- Print the commands a developer would run next: benchmark smoke, load dry-run,
  and fuzz corpus replay.
- Treat this as the bridge from tutorial code to release discipline.

## Full Listing (End of Lesson)

[lesson_20_performance_hardening.cpp](lesson_20_performance_hardening.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_20
.\build\debug\employee_directory_lesson_20.exe
```

## Expected Output

```
Objective: connect training code to benchmark, load, and fuzz evidence.
Found docs/PLANO_BENCHMARKS.md
Found docs/PLANO_TESTES_DE_CARGA.md
Found docs/FUZZING.md
Found tests/fuzz/corpus
Next commands:
  modb_bench run --profile smoke
  scripts/run-load.ps1 -Config loadtests/config/load-local.yaml -DryRun
  ctest -R modb.fuzz
```

## What to Notice

- Performance claims need captured environment and repeatable scenarios.
- Load tests and benchmarks are complementary, not interchangeable.
- Any parser that accepts bytes from disk or the network belongs in the fuzzing
  story.

## Next

Continue with [Lesson 21 -- Load Testing and Dashboard](../21-load-testing-dashboard/21-load-testing-dashboard.md).

## Related Reference

- [PLANO_BENCHMARKS.md](../../../PLANO_BENCHMARKS.md)
- [PLANO_TESTES_DE_CARGA.md](../../../PLANO_TESTES_DE_CARGA.md)
- [FUZZING.md](../../../FUZZING.md)
