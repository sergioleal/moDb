# Lesson 7 — Searching with Queries and Indexes

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

A search feature: find every employee in a department, or every employee
earning above some salary threshold, using `Database::query<Employee>()`
instead of hand-rolled loops. You'll add an index on `salary` and confirm
(via `.plan()`) that the search actually uses it.

## New Concepts

- The fluent `Query<T>` builder, lazy streaming via `.stream()`,
  `.where()`/`.limit()`/`.equals()`/`.between()` — see
  [docs/reference/queries-indexes.md](../../../reference/queries-indexes.md).
- `create_index<T>(field)` and the deterministic planner
  (`.plan()`/`first_result_cost`).

## Starting Point

The persistent database file after Lessons 1-6 have run.

## Steps

- Implement "employees earning at least 15000" with
  `.between(salary_field, AttributeValue{15000}, AttributeValue{max_int64})`,
  print `.plan()` — it reports `access=table_scan` because no index exists
  yet.
- Call `create_index<Employee>(salary_field)`, then run the *exact same*
  query again and print `.plan()` a second time — the access method
  changes to `index_scan` with no change to the query itself.
- Add a `top_k(2, comparator)` "top 2 earners" query and print the results
  with their salaries.

## Full Listing (End of Lesson)

[lesson_07_queries.cpp](lesson_07_queries.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_07
.\build\debug\employee_directory_lesson_07.exe
```

## Expected Output

```
Objective: search employees with a table scan, then an index, then top-k.
  employees earning >= 15000 (no index yet): access=table_scan index_available=false
  results: Carla
Created an index on Employee.salary
  employees earning >= 15000 (indexed): access=index_scan index_available=true
  results: Carla
Top 2 earners: Carla(20000) Ana(13250)
```

## What to Notice

- The planner is rule-based, not cost-based — the query shape didn't
  change at all between the two `.plan()` calls; only whether an index
  existed changed the outcome.
- `top_k` gets its own access path (Top-K selection) instead of a full
  sort, which is why it's listed alongside `.between()` rather than as a
  post-processing step.

## Related Reference

- [docs/reference/queries-indexes.md](../../../reference/queries-indexes.md)
- [USO_DA_CLI.md](../../../USO_DA_CLI.md) — `modb query ... --explain`
- [DEVELOPER_GUIDE.md, Chapter 8](../../../DEVELOPER_GUIDE.md#8-indexes-and-streaming-queries)
