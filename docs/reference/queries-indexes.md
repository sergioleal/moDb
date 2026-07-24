# Queries and Indexes

> Part of the `docs/reference/` set — see [docs/README.md](../README.md) for
> the full index and [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md) for a
> narrative, tutorial-style introduction. This document is a precise
> reference: it assumes you already know C++ well and want exact behavior,
> not a learning path.

## 1. Overview

`Database::query<T>()` returns a fluent, move-only builder. Nothing runs
until you call `.stream()` — at that point it becomes a lazy C++20 coroutine
generator, and storage is only touched as you actually iterate. There is no
query language string anywhere; every operator is a typed C++ method, and
the whole chain is checked at compile time.

The planner behind it is **deterministic, not cost-based** — there is no
cardinality estimation in this engine. A fixed set of rules decides access
method (table scan vs. index scan) and execution strategy (streaming,
partially-blocking Top-K, or fully-blocking sort/distinct), and the exact
same input always produces the exact same plan.

## 2. Concepts

### 2.1 The builder is one-shot and rvalue-qualified

```cpp
// include/modb/object/database.hpp:148-230 (abbreviated)
template <typename T>
class Query {
public:
    Query(const Query&) = delete;
    Query(Query&&) = default;
    Query& operator=(Query&&) = delete;

    Query&& where(std::function<bool(const T&)> predicate) &&;
    Query&& limit(std::size_t count) &&;
    Query&& cancel_on(query::CancellationToken token) &&;
    Query&& equals(FieldId field, AttributeValue value) &&;
    Query&& between(FieldId field, AttributeValue lo, AttributeValue hi) &&;
    Query&& order_by(std::function<bool(const T&, const T&)> less) &&;
    Query&& top_k(std::size_t k, std::function<bool(const T&, const T&)> less) &&;
    Query&& distinct_by(std::function<std::string(const T&)> key) &&;
    Query&& track_peak(std::size_t* peak) &&;

    [[nodiscard]] ProjectedQuery<T> select(std::vector<FieldId> fields) &&;
    [[nodiscard]] ProjectedQuery<T> compute(std::string name) &&;
    template <typename Out> [[nodiscard]] MappedQuery<Out> map(std::function<Out(const T&)> fn) &&;
    template <typename Acc, typename Fold>
    [[nodiscard]] query::Generator<Result<Acc>> aggregate(Acc initial, Fold fold) &&;

    [[nodiscard]] query::Generator<Result<T>> stream() &&;
    [[nodiscard]] query::QueryPlan plan() const;
    [[nodiscard]] query::OperatorNature nature() const noexcept;
    [[nodiscard]] std::size_t first_result_cost() const noexcept;
};
```

Every fluent method is `&&`-qualified — the whole chain has to be called on
an rvalue, which is exactly the pattern in every example
(`database->query<T>().where(...).limit(...).stream()`). You cannot store a
`Query<T>` in a named variable and call methods on it across statements; it
is meant to be built and consumed in one expression. `stream()` **moves the
underlying `Snapshot`** into the returned generator — the query's read view
is fixed at the moment `query<T>()` was called, same epoch semantics as
[reference/snapshots-mvcc.md](snapshots-mvcc.md).

### 2.2 Streaming means lazy, not "returns everything eventually"

```cpp
// examples/server/by_phase/phase_07/streaming_query.cpp:70-80
int count = 0;
for (auto& result : database->query<Item>()
                          .where([](const Item& item) { return item.value % 2 == 0; })
                          .limit(2)
                          .stream()) {
    if (!result) { std::cerr << result.error().message << '\n'; return 1; }
    std::cout << result->name << '\n';
    ++count;
}
```

`.stream()` returns `query::Generator<Result<T>>` — a coroutine-based lazy
sequence. Storage is read incrementally as the `for` loop advances; breaking
out of the loop early (after `limit(2)` is satisfied, or on your own
`break`) tears down the whole upstream coroutine chain cleanly. This is what
lets `limit` act as a true "stop early" signal instead of "compute
everything, then truncate" — but only for plans the planner classifies as
`streaming` (§2.3).

### 2.3 The planner: fixed rules, no cardinality estimation

```cpp
// include/modb/query/planner.hpp:66-117 (rule comment + logic, verbatim structure)
// 1. equals/between + index -> index_scan; otherwise table_scan (+ filter).
// 2. explicit top_k -> partially blocking.
// 3. order_by + limit (no distinct) -> selects Top-K, not a full sort.
// 4. order_by without limit -> blocking full sort.
// 5. distinct -> blocking.
// 6. limit on an otherwise-streaming plan -> pushed down to the source.
```

```cpp
struct QueryPlan {
    AccessMethod access{AccessMethod::table_scan}; // or index_scan
    OperatorNature nature{OperatorNature::streaming}; // or partially_blocking / blocking
    std::size_t first_result_cost{1};  // QueryPlan::kFullInput if the whole
                                        // input must be consumed before the
                                        // first result (TTFR cost)
    bool limit_pushed{false};
    bool uses_top_k{false};
    bool uses_sort{false};
    bool uses_distinct{false};
    bool index_requested{false};
    bool index_available{false};
    std::optional<std::uint16_t> index_field{};
};
```
(`include/modb/query/planner.hpp:17-52`)

Call `Query::plan()` (or the CLI's `--explain`, §7 of
[USO_DA_CLI.md](../USO_DA_CLI.md)) to see exactly which rule fired without
running the query. `first_result_cost == QueryPlan::kFullInput` is the
signal that a plan is not actually streaming in practice, regardless of
whether you called `.stream()` — `order_by` (without a `limit` that would
trigger Top-K selection) and `distinct_by` both force the whole input to be
consumed before the first result is yielded.

### 2.4 Indexes: `create_index<T>` and the underlying B+ tree

```cpp
// include/modb/object/database.hpp:679-718 (behavior, condensed)
template <typename T>
Result<void> create_index(FieldId field);
```

Builds a `BTree`, backfills every currently-stored object of type `T`, and
registers the index in the catalog — all inside its own internal
transaction (same "runs through the WAL, atomic and crash-safe" pattern as
`Database::bind`, [reference/object-model.md §3.4](object-model.md#34-databasebindt)).
Fails with `transaction_active` if called inside an open write transaction,
`type_not_found` if `T` isn't bound yet.

Once an index exists on a field, `.equals(field, value)` or
`.between(field, lo, hi)` on a query over that type use it automatically —
there is no separate "use index X" call; the planner checks catalog
availability for you (`index_available` in `QueryPlan`).

The underlying `BTree` (`include/modb/index/btree.hpp:26-56`) is not
something application code touches directly, but its shape is worth
knowing: keys are the field's encoded value bytes **plus a trailing
`ObjectId` tie-breaker**, so duplicate values are fully supported and
stably ordered by insertion identity. `insert`/`remove` operate on that
composite key; `find` does exact-value lookup, `range` does inclusive
`[lo, hi]` range scan, both returning `ObjectId`s in ascending order.

## 3. API Reference

### 3.1 `Database::query<T>()` → `Query<T>`

Opens a `Snapshot` at the current epoch and returns the fluent builder
described in §2.1.

### 3.2 Filtering and limiting

- `where(predicate)` — keeps only materialized values the predicate accepts.
- `limit(count)` — `0` means unlimited. On a streaming plan, pushed to the
  source; on sort/top_k/distinct, applied after that operator.
- `cancel_on(token)` — cooperative cancellation; the stream ends when the
  token is signaled.

### 3.3 Index-backed predicates

- `equals(field, value)` / `between(field, lo, hi)` — become an index scan
  if `create_index<T>(field)` was called for that field; otherwise the
  planner falls back to a table scan (still correct, just not
  index-accelerated).

### 3.4 Ordering, Top-K, and distinct

- `order_by(less)` — full blocking sort by a caller-supplied strict weak
  ordering.
- `top_k(k, less)` — partially-blocking: the k "largest" elements per
  `less`, without a full sort. The planner also auto-selects this strategy
  when you combine `order_by` with `limit` and no `distinct_by` (rule 3,
  §2.3) — you don't have to call `top_k` explicitly for that common case.
- `distinct_by(key)` — first occurrence per string key, blocking.

### 3.5 Projection, computed fields, and mapping

- `select(fields)` → `ProjectedQuery<T>` — only the requested `FieldId`s,
  materialized as a `ProjectedRow` instead of a full `T`.
- `compute(name)` → `ProjectedQuery<T>` — a named computed function
  registered on the `Database` (see `Database::register_computed_function`
  in the header, sibling to `create_index`).
- `map(fn)` → `MappedQuery<Out>` — typed, element-wise transform, either
  infallible (`Out(const T&)`) or fallible (`Result<Out>(const T&)`).

### 3.6 Aggregation

- `aggregate(initial, fold)` → `query::Generator<Result<Acc>>` — blocking
  reduction of the whole stream into a single accumulator.

### 3.7 Terminal operations

- `stream() &&` → `query::Generator<Result<T>>` — consumes the `Query`,
  moving its `Snapshot` into the generator.
- `plan() const` → `query::QueryPlan` — inspect without running.
- `nature() const noexcept` → `query::OperatorNature` (delegates to `plan()`).
- `first_result_cost() const noexcept` → `std::size_t` (delegates to
  `plan()`; the TTFR — time-to-first-result — proxy metric).

### 3.8 `Database::create_index<T>(FieldId)` → `Result<void>`

`database.hpp:679-718` — see §2.4.

### 3.9 `index::BTree` (advanced / internal-facing)

`include/modb/index/btree.hpp:26-56`

```cpp
static Result<BTree> create(storage::PageFile&);
static Result<BTree> open(storage::PageFile&, storage::PageId root);
[[nodiscard]] storage::PageId root_page() const noexcept;
Result<void> insert(std::span<const std::byte> key, std::uint64_t object_id);
Result<void> remove(std::span<const std::byte> key, std::uint64_t object_id);
[[nodiscard]] Result<std::vector<std::uint64_t>> find(std::span<const std::byte> key) const;
[[nodiscard]] Result<std::vector<std::uint64_t>> range(std::span<const std::byte> lo,
                                                       std::span<const std::byte> hi) const;
[[nodiscard]] Result<std::uint32_t> validate() const; // structural self-check, used by tests
```

## 4. Semantics & Invariants

- `.stream()` (and every other terminal/fluent method) is rvalue-qualified —
  the builder chain must be used in a single expression.
- The read view is fixed at `query<T>()` construction time (a `Snapshot`
  under the hood), not at `.stream()` time.
- Planner rules are fixed and deterministic — no statistics, no
  environment-dependent plan changes for the same query shape.
- `order_by` alone or `distinct_by` force `first_result_cost == kFullInput` —
  they are not streaming in the TTFR sense even though you still get a
  `Generator`.
- `create_index<T>` requires the type already bound and no open write
  transaction; it runs atomically through the WAL.
- B+ tree keys are value bytes + a trailing `ObjectId` — duplicate field
  values are always representable and stably ordered.

## 5. Common Pitfalls

- **Assuming every query is O(1) memory.** `order_by`/`distinct_by` (and
  `top_k`/auto-selected Top-K) all materialize some or all of the input
  before yielding — check `plan().first_result_cost` if this matters to you.
- **Trying to reuse a `Query<T>` object across two `.stream()` calls.** Not
  supported — the type is designed for one fluent expression.
- **Expecting `.equals()`/`.between()` to auto-create an index.** They
  don't — you must call `create_index<T>(field)` yourself first; without
  it, the same predicate still works, just via table scan.
- **Calling `create_index` inside a write transaction.** Fails with
  `transaction_active` — call it standalone, like `bind()`.

## 6. Worked Example

Full round trip — seed, index, query with `--explain`-equivalent inspection
— condensed from `examples/server/by_phase/phase_07/streaming_query.cpp` and
the CLI transcripts in [USO_DA_CLI.md](../USO_DA_CLI.md):

```cpp
struct Item { std::string name; std::int64_t value{}; };
// ... bind Item with field<1>(name), field<2>(value) ...

auto tx = database->begin();
for (int i = 0; i < 6; ++i) {
    database->create(*tx, Item{"item-" + std::to_string(i), i});
}
tx->commit();

int count = 0;
for (auto& result : database->query<Item>()
                          .where([](const Item& item) { return item.value % 2 == 0; })
                          .limit(2)
                          .stream()) {
    if (!result) { return 1; }
    std::cout << result->name << '\n'; // item-0, item-2
    ++count;
}
// count == 2; the loop stopped after two matches without scanning the rest.
```

Inspecting the plan for an index-backed query (mirrors `modb query ... --explain`):

```cpp
database->create_index<Employee>(FieldId{2}); // index on `salary`
auto q = database->query<Employee>().equals(FieldId{2}, AttributeValue{18000});
auto plan = q.plan();
// plan.access == AccessMethod::index_scan
// plan.nature == OperatorNature::streaming
// plan.first_result_cost == 1
```

```text
$ modb query phase3.modb --schema 2 --salary 18000 --limit 1 --explain
plan: access=index_scan nature=streaming first_result_cost=1 limit_pushed=true index_requested=true index_available=true
Employee: name=Bia salary=18000 country=PT
1 employee(s) streamed (via index) (nature=streaming); data pages read: 0
```

## 7. Related Documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 8
- [reference/snapshots-mvcc.md](snapshots-mvcc.md) — the read view a query
  is anchored to
- [USO_DA_CLI.md](../USO_DA_CLI.md) — `modb query ... --explain` and the
  full CLI query surface

## 8. Related Source

- `include/modb/object/database.hpp` (`Query<T>`, `ProjectedQuery<T>`,
  `MappedQuery<T>`, `create_index`)
- `include/modb/query/generator.hpp`, `planner.hpp`, `operators.hpp`,
  `projected_row.hpp`
- `include/modb/index/btree.hpp`, `key_codec.hpp`
- `examples/server/by_phase/phase_07/streaming_query.cpp`
- `tests/streaming_query_tests.cpp`, `tests/indexed_query_test.cpp`,
  `tests/planner_test.cpp`, `tests/projection_query_test.cpp`,
  `tests/aggregation_query_test.cpp`
