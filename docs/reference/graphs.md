# Graphs

> **Status:** 🚧 skeleton — outline only, not yet written. In the meantime see
> [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 12.

## Overview

- TODO: typed traversal over `Ref`/`OwnedRef` edges, anchored to a `Snapshot`
  the caller keeps alive.
- TODO: lazy traversal (same generator model as streaming queries) vs. the
  small, non-persisted `EdgeHandle` view.

## Core API surface

- TODO `open_graph_view(Database&, const Snapshot&)` —
  `include/modb/graph/graph_view.hpp:153-154`
- TODO `GraphView::outgoing_collection<From, To>(...)` — `graph_view.hpp:55-58`
- TODO `GraphView::incoming<From, To>(...)` — `graph_view.hpp:110-113`
  (requires a B+ index on the pointer field — no reverse-scan fallback)
- TODO `EdgeHandle<From, To, Kind>::source/target/dangling` —
  `include/modb/graph/edge_handle.hpp:22-62`
- TODO `graph::bfs` / `graph::dfs` — `include/modb/graph/traversal.hpp:94-97,159-162`
  (lazy `query::Generator<Result<GraphVisit>>`)
- TODO `TraversalOptions{max_depth, max_vertices, dangling}` —
  `traversal.hpp:32-40`
- TODO `shortest_path` / `has_cycle` / `topological_sort` /
  `connected_components` — `include/modb/graph/algorithms.hpp`

## Semantics & invariants

- TODO: `incoming()` without an index → `invalid_edge`.
- TODO: `target()` on a dangling edge → `edge_target_not_found`
  (`record_not_found` translated).
- TODO: `shortest_path` unreachable → `record_not_found`;
  `topological_sort` on a cycle → `graph_cycle`; any limit exceeded →
  `graph_limit_exceeded`.
- TODO: `connected_components` needs an adjacency function that already
  returns both directions.

## Common pitfalls

- TODO: letting the anchoring `Snapshot` go out of scope while still using a
  `GraphView`/`EdgeHandle` derived from it.
- TODO: calling `incoming()` on a field with no index.

## Related documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 12
- [ADR-018](../decisions/ADR-018-handles-de-arestas-e-algoritmos-de-grafos.md)

## Related source

- `include/modb/graph/graph_view.hpp`, `edge_handle.hpp`, `traversal.hpp`,
  `algorithms.hpp`
- `examples/server/by_phase/phase_12/graph_traversal.cpp`
- `tests/graph_view_test.cpp`, `tests/graph_algorithms_test.cpp`,
  `tests/edge_handle_test.cpp`
