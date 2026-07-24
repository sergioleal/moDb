# Lesson 11 — The Org Chart: Graphs

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

A `manager: Ref<Employee>` field (an employee pointing at another employee)
and an org-chart feature: "list everyone who (directly or indirectly)
reports to X," implemented as a lazy BFS instead of hand-written recursive
queries.

## New Concepts

- `GraphView`/`open_graph_view`, `EdgeHandle`, `graph::bfs`/`dfs`,
  `TraversalOptions`/`DanglingPolicy` — see
  [docs/reference/graphs.md](../../reference/graphs.md).
- Why `incoming()` (find an employee's direct reports) needs an index on
  `manager`.

## Starting Point

Lesson 10's server/client with `HRFacade`.

## Steps

- Add `manager: Ref<Employee>` (field 7) to `Employee`, defaulted to a
  zero-valued `Ref<Employee>{}` — the fourth schema evolution this run
  (after Lesson 4's `country` and Lesson 5's three relationship fields) —
  and `create_index<Employee>(manager_field)`, a prerequisite for
  `incoming()`.
- Assign Ana and Bruno's `manager` to Carla via `Handle::set`.
- Open a `Snapshot`, `graph::open_graph_view(database, snapshot)`, and
  build an `AdjacencyFn` that wraps `GraphView::incoming` (an employee's
  direct reports). Run `graph::bfs(carla_id, adjacency_down)` and print
  the org chart with depths.
- Create a temporary employee reporting to another temporary employee,
  remove the manager, and walk *upward* from the report (this direction
  uses `graph::edge()` over the single `manager` field, not `incoming()`
  — `incoming()` only ever surfaces reports that still resolve). Run the
  same walk three times with `DanglingPolicy::fail`, `skip`, and
  `yield_error` and compare what each one does with the now-dangling edge.

## Full Listing (End of Lesson)

[examples/employee_directory/lesson_11_graphs.cpp](../../../examples/employee_directory/lesson_11_graphs.cpp)
— embedded, no server/client split needed to demonstrate the graph API
itself.

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_11
.\build\debug\employee_directory_lesson_11.exe
```

## Expected Output

```
Objective: walk the reporting chain with the graph module.
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
Lesson 8: handshake ok, protocol 1.0, max_concurrent_streams=4
Lesson 8: remote query returned 3 employees
Lesson 8: remote search for salary == 20000 returned 1 match(es): Carla
Lesson 9: transferring Bruno to Engineering (his old department, Sales, was removed back in Lesson 5)
  succeeded
Lesson 9: attempting a transfer to a department that doesn't exist
  failed as expected: target department does not exist
Lesson 10: opening the "hr" facade (version 1)
Lesson 10: moving Bruno back to Engineering through HRFacade::TransferDepartment
  succeeded
Lesson 10: giving Ana a raise through HRFacade::GiveRaise
  succeeded
Lesson 10: a client compiled against "hr" version 2 asks for a version the server never published
  rejected as expected: incompatible facade version for 'hr': requested 2
Lesson 11: Ana and Bruno now report to Carla
Lesson 11: org chart under Carla (breadth-first)
  depth 0: Carla
  depth 1: Ana
  depth 1: Bruno
Lesson 11: removed Gustavo's manager Felipe (39) -- Gustavo's `manager` field now points nowhere
Lesson 11: walking up from Gustavo with DanglingPolicy::fail
  visit 40 depth=0
  stopped: no object with id 39
Lesson 11: walking up from Gustavo with DanglingPolicy::skip
  visit 40 depth=0
  (walk ended quietly -- the dangling edge was dropped, not reported)
Lesson 11: walking up from Gustavo with DanglingPolicy::yield_error
  visit 40 depth=0
  error item: no object with id 39
  (unlike fail, yield_error would have let the walk continue into any other, unrelated branch -- Gustavo only had the one dangling edge, so the visible output matches fail's here)
```

## What to Notice

- `GraphView` doesn't own its `Snapshot` — it holds a pointer, so the
  snapshot must outlive any view or edge handle derived from it.
- `incoming()` has no reverse-scan fallback; without an index on the `Ref`
  field it fails outright (`invalid_edge`) rather than silently scanning
  everything. It also can't produce a "dangling" result — a stale or
  missing source is just skipped, which is why the dangling-policy demo
  above walks *upward* via `graph::edge()` on the single `manager` field
  instead.
- With a singular `manager` field, `fail` and `yield_error` look identical
  in this particular run: both surface the same error for Gustavo's one
  dangling edge. The difference only becomes visible when a node has
  multiple neighbors and only some of them are dangling — `yield_error`
  keeps exploring the others, `fail` aborts the whole traversal.

## Related Reference

- [docs/reference/graphs.md](../../reference/graphs.md)
- [docs/reference/relationships-collections.md](../../reference/relationships-collections.md)
- [DEVELOPER_GUIDE.md, Chapter 12](../../DEVELOPER_GUIDE.md#12-graphs)
