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
  [docs/reference/queries-indexes.md](../../reference/queries-indexes.md).
- `create_index<T>(field)` and the deterministic planner
  (`.plan()`/`first_result_cost`).

## Starting Point

Lesson 6's directory with the payroll report and compaction command.

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

[examples/employee_directory/lesson_07_queries.cpp](../../../examples/employee_directory/lesson_07_queries.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_07
.\build\debug\employee_directory_lesson_07.exe
```

## Expected Output

```
Objective: search employees with a table scan, then an index, then top-k.
Lesson 1: Employee type id = 16
Lesson 2: wrote 3 employees (Ana=18, Bruno=19, Carla=20)
Lesson 2: after reopen, employee 18 = Ana (12000)
Lesson 3: committed raise for Bruno
  Bruno after committed raise: Bruno = 10500
Lesson 3: uncommitted raise for Carla (deliberately not committed)
  Carla after scope exit (should be unchanged): Carla = 15000
Lesson 3: averaging Ana and Bruno's salaries in one transaction
  Ana after averaging: Ana = 11250
  Bruno after averaging: Bruno = 11250
Lesson 3: attempting a second transaction while one is open
  second begin() failed as expected: a transaction is already in progress
Lesson 4: Ana read through the new binding = Ana (11250, country=BR) -- country came from the declared default, not from disk
Lesson 4: raise for Ana via Handle::set (not manual materialize/update)
  Ana after Handle::set raise: Ana (13250, country=BR) -- now physically stored in the new 3-field shape
Lesson 5: created departments Engineering=31, Sales=32
Lesson 5: assigned Ana -> Engineering, Bruno -> Sales
Lesson 5: Diego=34 has emergency contact 33
Lesson 5: after removing Diego, his emergency contact is gone too (cascade-deleted): no object with id 33
Lesson 5: Carla's projects: Phoenix Atlas
Lesson 5: after removing Sales, resolving it directly fails as expected: no object with id 32 -- Bruno's `department` field still holds that (now dangling) id
Lesson 6: payroll total at the snapshot's epoch = 39500
Lesson 6: payroll total at the SAME snapshot after Carla's raise = 39500 (unchanged -- the snapshot doesn't see it)
Lesson 6: second raise while the snapshot is open failed as expected: object 20 already has a previous version visible to an older open snapshot
Lesson 6: retried raise for Carla succeeded once the snapshot closed
Lesson 6: payroll total on a fresh snapshot = 44500
Lesson 6: collect_garbage() reclaimed 12 record(s)
  employees earning >= 15000 (no index yet): access=table_scan index_available=false
  results: Carla
Lesson 7: created an index on Employee.salary
  employees earning >= 15000 (indexed): access=index_scan index_available=true
  results: Carla
Lesson 7: top 2 earners: Carla(20000) Ana(13250)
```

## What to Notice

- The planner is rule-based, not cost-based — the query shape didn't
  change at all between the two `.plan()` calls; only whether an index
  existed changed the outcome.
- `top_k` gets its own access path (Top-K selection) instead of a full
  sort, which is why it's listed alongside `.between()` rather than as a
  post-processing step.

## Related Reference

- [docs/reference/queries-indexes.md](../../reference/queries-indexes.md)
- [USO_DA_CLI.md](../../USO_DA_CLI.md) — `modb query ... --explain`
- [DEVELOPER_GUIDE.md, Chapter 8](../../DEVELOPER_GUIDE.md#8-indexes-and-streaming-queries)
