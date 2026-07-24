# Lesson 11 — The Org Chart: Graphs

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

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

- TODO: add `manager: Ref<Employee>` to `Employee`, and an index on it (a
  prerequisite for `incoming()`, per
  [docs/reference/queries-indexes.md](../../reference/queries-indexes.md)).
- TODO: build an `AdjacencyFn` from `GraphView::incoming` (direct reports of
  a given manager).
- TODO: run `graph::bfs(top_manager_id, adjacency)` and print the whole
  chain of command with depth.
- TODO: introduce a dangling `manager` reference (remove a manager without
  reassigning reports) and show the three `DanglingPolicy` behaviors
  (`fail`/`skip`/`yield_error`) side by side.

## Full Listing (End of Lesson)

TODO — target: `examples/employee_directory/lesson_11_org_chart.cpp`
(embedded; doesn't need the server/client split to demonstrate the graph
API itself).

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_11
.\build\debug\employee_directory_lesson_11.exe
```

## Expected Output

TODO — a printed org chart with depths, and the three dangling-manager
behaviors compared.

## What to Notice

- TODO: `GraphView` doesn't own its `Snapshot` — keep the snapshot alive as
  long as you're using a view or edge handle derived from it.
- TODO: `incoming()` has no reverse-scan fallback; without the index it
  fails outright rather than silently scanning everything.

## Related Reference

- [docs/reference/graphs.md](../../reference/graphs.md)
- [docs/reference/relationships-collections.md](../../reference/relationships-collections.md)
- [DEVELOPER_GUIDE.md, Chapter 12](../../DEVELOPER_GUIDE.md#12-graphs)
