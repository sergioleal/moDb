# Queries and Indexes

> **Status:** 🚧 skeleton — outline only, not yet written. In the meantime see
> [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 8.

## Overview

- TODO: `Query<T>` as a lazy, fluent, one-shot builder; what "streaming"
  means (a coroutine generator, storage touched only while iterating).
- TODO: the planner is deterministic (rule-based), not cost-based — no
  cardinality estimation.

## Core API surface

- TODO `Database::query<T>()` → `Query<T>` — `include/modb/object/database.hpp:621-633`
- TODO `Query::where(...)`, `Query::limit(...)` — `database.hpp:157-168`
- TODO `Query::equals(FieldId, AttributeValue)`, `Query::between(...)` —
  `database.hpp:176-188`
- TODO `Query::stream() &&` → `query::Generator<Result<T>>` — `database.hpp:226`
- TODO `Query::plan() const` → `query::QueryPlan` — `database.hpp:229`
- TODO `Database::create_index<T>(FieldId)` — `database.hpp:679-718`
- TODO `query::Generator<T>` — `include/modb/query/generator.hpp`
- TODO planner rules — `include/modb/query/planner.hpp:73-117`
  (`plan_query(QueryIntent) -> QueryPlan`)
- TODO `BTree` public API (not internals) — `include/modb/index/btree.hpp:28-60`
  (`create`, `open`, `insert`, `remove`, `find`, `range`)

## Semantics & invariants

- TODO: `.stream()` is rvalue-qualified — the builder chain is one-shot.
- TODO: index keys are value bytes + a trailing `ObjectId` tie-breaker —
  duplicates allowed, stably ordered.
- TODO: planner rule table — equals/between+index → index scan;
  order_by+limit → Top-K; order_by alone or distinct → full blocking sort;
  plain limit on a streaming plan → pushed to the source.
- TODO: `create_index<T>` requires the type already bound, and no open write
  transaction.

## Common pitfalls

- TODO: assuming every query stays O(1) memory — `order_by`/`distinct`
  materialize the input.
- TODO: reusing a `Query` object after `.stream()` (not supported, one-shot).

## Related documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 8
- [USO_DA_CLI.md](../USO_DA_CLI.md) — `modb query ... --explain`

## Related source

- `include/modb/query/generator.hpp`, `planner.hpp`, `operators.hpp`, `projected_row.hpp`
- `include/modb/index/btree.hpp`, `key_codec.hpp`
- `examples/server/by_phase/phase_07/streaming_query.cpp`
- `tests/streaming_query_tests.cpp`, `tests/indexed_query_test.cpp`,
  `tests/planner_test.cpp`
