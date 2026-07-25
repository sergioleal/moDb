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
  [docs/reference/graphs.md](../../../reference/graphs.md).
- Why `incoming()` (find an employee's direct reports) needs an index on
  `manager`.

## Starting Point

The persistent database file after Lessons 1-10 have run.

## Steps

- Look Ana, Bruno, and Carla up by name.
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

[lesson_11_graphs.cpp](lesson_11_graphs.cpp)
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
Ana and Bruno now report to Carla
Org chart under Carla (breadth-first)
  depth 0: Carla
  depth 1: Ana
  depth 1: Bruno
Removed Gustavo's manager Felipe (39) -- Gustavo's `manager` field now points nowhere
Walking up from Gustavo with DanglingPolicy::fail
  visit 40 depth=0
  stopped: no object with id 39
Walking up from Gustavo with DanglingPolicy::skip
  visit 40 depth=0
  (walk ended quietly -- the dangling edge was dropped, not reported)
Walking up from Gustavo with DanglingPolicy::yield_error
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

- [docs/reference/graphs.md](../../../reference/graphs.md)
- [docs/reference/relationships-collections.md](../../../reference/relationships-collections.md)
- [DEVELOPER_GUIDE.md, Chapter 12](../../../DEVELOPER_GUIDE.md#12-graphs)
