# Lesson 7 — Searching with Queries and Indexes

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

A search feature: find every employee in a department, or every employee
earning above some salary threshold, using `Database::query<Employee>()`
instead of hand-rolled loops. You'll add an index on `salary` and confirm
(via `.plan()`) that the search actually uses it.

## New Concepts

- The fluent `Query<T>` builder, lazy streaming via `.stream()`,
  `.where()`/`.limit()`/`.equals()`/`.between()` — see
  [docs/reference/queries-indexes.md](../../reference/queries-indexes.md).
- `create_index<T>(field)` and the deterministic planner
  (`.plan()`/`first_result_cost`).

## Starting Point

Lesson 6's directory with the payroll report and compaction command.

## Steps

- TODO: implement "employees in department X" with `.where(...)`.
- TODO: implement "employees earning at least N" first as a table scan,
  print `.plan()`, then call `create_index<Employee>(salary_field)` and
  print `.plan()` again to show the access method change to `index_scan`.
- TODO: add an `order_by` + `limit` "top 5 earners" query and print
  `.plan()` to see Top-K selection kick in instead of a full sort.
- TODO: add a `distinct_by` "list of departments in use" query.

## Full Listing (End of Lesson)

TODO — target: `examples/employee_directory/lesson_07_queries.cpp`.

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_07
.\build\debug\employee_directory_lesson_07.exe
```

## Expected Output

TODO — plan output changing from `table_scan` to `index_scan` after
`create_index`, plus the Top-K/distinct query results.

## What to Notice

- TODO: the planner is rule-based, not cost-based — the same query shape
  always produces the same plan.
- TODO: `order_by` (without the Top-K auto-selection) and `distinct_by` are
  not streaming in the TTFR sense, even though you still get a generator.

## Related Reference

- [docs/reference/queries-indexes.md](../../reference/queries-indexes.md)
- [USO_DA_CLI.md](../../USO_DA_CLI.md) — `modb query ... --explain`
- [DEVELOPER_GUIDE.md, Chapter 8](../../DEVELOPER_GUIDE.md#8-indexes-and-streaming-queries)
