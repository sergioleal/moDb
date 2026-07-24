# Graphs

> Part of the `docs/reference/` set — see [docs/README.md](../README.md) for
> the full index and [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md) for a
> narrative, tutorial-style introduction. This document is a precise
> reference: it assumes you already know C++ well and want exact behavior,
> not a learning path.

## 1. Overview

Ring0 doesn't have a separate "graph database" mode — it has typed,
runtime-only views over the `Ref`/`OwnedRef` relationships you already
declared in your bindings ([reference/relationships-collections.md](relationships-collections.md)),
plus lazy traversal/algorithm helpers that work over any adjacency function
you supply. Nothing here is persisted differently: `GraphView`/`EdgeHandle`
are thin, non-owning wrappers computed on demand from a `Snapshot`.

## 2. Concepts

### 2.1 `GraphView` — anchored to a `Snapshot`, not owning it

```cpp
// include/modb/graph/graph_view.hpp:42-51
class GraphView {
public:
    GraphView(object::Database& database, const object::Snapshot& snapshot);
    // ...
};
[[nodiscard]] Result<GraphView> open_graph_view(object::Database& database,
                                                const object::Snapshot& snapshot);
```

`GraphView` stores a `Database*` and a `const Snapshot*` — it does **not**
extend the snapshot's lifetime. If the `Snapshot` you opened it with is
destroyed while the `GraphView` (or an `EdgeHandle` derived through it) is
still around, you have a dangling reference — this is a sharp edge worth
remembering, unlike everywhere else in this reference set where lifetime is
managed through `DatabaseId`/registry lookups.

### 2.2 Outgoing edges from a `PersistentVector<Ref<To>>` collection

```cpp
// include/modb/graph/graph_view.hpp:55-106 (behavior, condensed)
template <class From, class To>
Result<std::vector<EdgeHandle<From, To, EdgeKind::association>>>
outgoing_collection(const Handle<From>& source, FieldId field, BlobId From::* member) const;
```

This resolves the parent's `BlobId` field (validating it really is a
`PersistentVector` blob field bound at that `FieldId`, not just any blob),
opens the `PersistentVector<Ref<To>>` over it, and returns one
`EdgeHandle` per element, preserving the collection's element order.

### 2.3 Incoming edges require an index — there is no reverse scan

```cpp
// include/modb/graph/graph_view.hpp:110-146 (behavior, condensed)
template <class From, class To>
Result<std::vector<EdgeHandle<From, To, EdgeKind::association>>>
incoming(const Handle<To>& target, FieldId field, Ref<To> From::* member) const;
```

This looks up `From` objects whose `field` points at `target` via
`Database::indexed_object_ids<From>(field, AttributeValue{target.id()})` —
which means [an index must already exist](queries-indexes.md#24-indexes-create_indext-and-the-underlying-b-tree)
on that field. There is deliberately no fallback full scan: without an
index, `incoming()` fails with `invalid_edge` rather than silently doing an
expensive reverse scan you didn't ask for. Each candidate is also
re-checked against the live snapshot value (in case the index entry is
stale relative to the snapshot's epoch) before being included.

### 2.4 A single `Ref`/`OwnedRef` field as one edge: the `edge()` function

Beyond collections, a single relationship field can be resolved directly:

```cpp
// include/modb/graph/edge_handle.hpp:121-156, 158-193
template <class From, class To>
Result<EdgeHandle<From, To, EdgeKind::association>>
edge(Database& database, const Handle<From>& source, FieldId field, Ref<To> From::* member);

template <class From, class To>
Result<EdgeHandle<From, To, EdgeKind::ownership>>
edge(Database& database, const Handle<From>& source, FieldId field, OwnedRef<To> From::* member);
```

Both overloads validate the given `FieldId` really is bound as a `Ref`
(association) or `OwnedRef` (ownership) field respectively
(`detail::validate_edge_field`) before constructing the `EdgeHandle` — this
is a distinct `EdgeKind` from the collection-based edges in §2.2, mirroring
whether the underlying member is `Ref<T>` or `OwnedRef<T>`.

### 2.5 `EdgeHandle<From, To, Kind>` — a non-persisted runtime view

```cpp
// include/modb/graph/edge_handle.hpp:17-88
enum class EdgeKind : std::uint8_t { association = 0, ownership = 1 };

template <class From, class To, EdgeKind Kind = EdgeKind::association>
class EdgeHandle {
public:
    [[nodiscard]] DatabaseId database() const noexcept;
    [[nodiscard]] ObjectId source_id() const noexcept;
    [[nodiscard]] ObjectId target_id() const noexcept;
    [[nodiscard]] FieldId field() const noexcept;
    [[nodiscard]] Result<From> source(const Snapshot&) const;
    [[nodiscard]] Result<To> target(const Snapshot&) const;   // edge_target_not_found if dangling
    [[nodiscard]] Result<bool> dangling(const Snapshot&) const;
};
```

`target()` translates a plain `record_not_found` into the more specific
`edge_target_not_found` — so code that specifically cares about dangling
edges (as opposed to some other resolution failure) can match on that exact
code. `dangling()` is just `target()` re-interpreted: `true` only for that
specific error, any other error still propagates as a failure.

### 2.6 Lazy traversal: `bfs`/`dfs` over any adjacency function

```cpp
// include/modb/graph/traversal.hpp:17-45
enum class DanglingPolicy : std::uint8_t {
    fail = 0,        // first orphaned neighbor aborts the traversal with an error
    skip = 1,        // silently skip the orphaned neighbor and continue
    yield_error = 2,  // yield a failed Result and continue if possible
};
struct GraphVisit { ObjectId id{}; std::uint32_t depth{0}; };
struct TraversalOptions {
    std::uint32_t max_depth{0};    // 0 = unlimited
    std::uint64_t max_vertices{0}; // 0 = unlimited
    DanglingPolicy dangling{DanglingPolicy::fail};
    query::CancellationToken cancel{};
    bool has_cancel{false};
};
using AdjacencyFn = std::function<Result<std::vector<ObjectId>>(ObjectId from)>;
using ResolveFn = std::function<Result<bool>(ObjectId target)>;

query::Generator<Result<GraphVisit>> bfs(ObjectId start, AdjacencyFn adjacency,
                                         TraversalOptions options = {}, ResolveFn resolve = {});
query::Generator<Result<GraphVisit>> dfs(ObjectId start, AdjacencyFn adjacency,
                                         TraversalOptions options = {}, ResolveFn resolve = {});
```

`bfs`/`dfs` know nothing about `Database`/`GraphView` — they operate purely
on `ObjectId`s and a caller-supplied `AdjacencyFn`. This is deliberate
separation of concerns: the traversal engine is the same lazy
`query::Generator` machinery as streaming queries
([reference/queries-indexes.md §2.2](queries-indexes.md#22-streaming-means-lazy-not-returns-everything-eventually)),
and you decide what "adjacency" means — pulling from `GraphView`, from an
in-memory map (as in the worked example, §6), or from anywhere else.

### 2.7 Algorithms built on the same `AdjacencyFn`

```cpp
// include/modb/graph/algorithms.hpp (signatures)
Result<std::vector<ObjectId>> shortest_path(ObjectId start, ObjectId goal, AdjacencyFn adjacency,
                                            TraversalOptions options = {}, ResolveFn resolve = {});
Result<bool> has_cycle(const std::vector<ObjectId>& vertices, AdjacencyFn adjacency,
                       TraversalOptions options = {}, ResolveFn resolve = {});
Result<std::vector<ObjectId>> topological_sort(const std::vector<ObjectId>& vertices,
                                               AdjacencyFn adjacency, TraversalOptions options = {},
                                               ResolveFn resolve = {});
Result<std::vector<std::vector<ObjectId>>> connected_components(
    const std::vector<ObjectId>& vertices, AdjacencyFn adjacency, TraversalOptions options = {},
    ResolveFn resolve = {});
```

`connected_components` needs an `AdjacencyFn` that already returns **both**
directions (i.e., an undirected view) — it doesn't infer reverse edges for
you.

## 3. API Reference

### 3.1 `open_graph_view` / `GraphView` (see §2.1–2.3)

### 3.2 `edge()` (see §2.4)

### 3.3 `EdgeHandle<From, To, Kind>` (see §2.5)

### 3.4 `bfs` / `dfs` (see §2.6)

### 3.5 Algorithms (see §2.7)

Error codes worth knowing: `shortest_path` returns `record_not_found` if
`goal` is unreachable; `topological_sort` returns `graph_cycle` on a cyclic
input; any traversal/algorithm exceeding `TraversalOptions::max_depth` /
`max_vertices` returns `graph_limit_exceeded`; a cooperative cancellation
via `TraversalOptions::cancel` surfaces as `invalid_argument` with a
"cancelled" message.

## 4. Semantics & Invariants

- `GraphView` does not own the `Snapshot` it's constructed with — the
  snapshot must outlive the view (and any `EdgeHandle`s produced through it
  that you intend to resolve later).
- `incoming()` requires a B+ index on the pointer field; there is no
  fallback reverse scan.
- `EdgeKind::association` (`Ref`) vs `EdgeKind::ownership` (`OwnedRef`) is
  validated against the actual bound field — asking for the wrong kind on a
  given `FieldId` fails with `invalid_edge`, not a silent mismatch.
- `target()`/`dangling()` distinguish "target doesn't exist"
  (`edge_target_not_found`) from other kinds of resolution failure.
- `bfs`/`dfs`/algorithms are agnostic to where adjacency data comes from —
  they only need an `AdjacencyFn`.
- `DanglingPolicy` controls what happens per-neighbor when `resolve()`
  reports it doesn't exist: `fail` aborts the whole traversal, `skip`
  silently excludes it, `yield_error` surfaces one failed `Result` for that
  neighbor and (where possible) continues.

## 5. Common Pitfalls

- **Letting the anchoring `Snapshot` go out of scope while still using a
  `GraphView`/`EdgeHandle` derived from it.** Nothing in the type system
  stops you — this is the one relationship-adjacent lifetime hazard that
  isn't managed via `DatabaseRegistry`.
- **Calling `incoming()` on a field with no index and expecting it to just
  work slowly.** It fails outright (`invalid_edge`) instead — create the
  index first.
- **Passing a `Ref`-only adjacency function to `connected_components`.**
  You'll get weakly-connected-in-one-direction results unless your
  `AdjacencyFn` already includes both directions.
- **Assuming `dangling()` is a stored flag.** It's computed fresh on every
  call by attempting resolution — not a cached bit anywhere on disk.

## 6. Worked Example

Lazy BFS over a plain in-memory adjacency map (the traversal engine doesn't
care where adjacency comes from), condensed from
`examples/server/by_phase/phase_12/graph_traversal.cpp`:

```cpp
using modb::object::ObjectId;
std::unordered_map<std::uint64_t, std::vector<ObjectId>> edges{
    {1, {ObjectId{2}, ObjectId{3}}},
    {2, {ObjectId{4}}},
    {3, {}},
    {4, {}},
};

auto adjacency = [&edges](ObjectId from) -> modb::Result<std::vector<ObjectId>> {
    return edges[from.value];
};

// bfs() streams traversal items — callers can stop early if needed.
for (auto& item : modb::graph::bfs(ObjectId{1}, adjacency)) {
    if (!item) { std::cerr << item.error().message << '\n'; return 1; }
    std::cout << "visit " << item->id.value << " depth=" << item->depth << '\n';
}
// Visits 1 (depth 0), 2 and 3 (depth 1), 4 (depth 2).
```

Adjacency backed by real `Ref` relationships through `GraphView` (composed
from §2.2–2.4; illustrative, adapt field ids/types to your own schema):

```cpp
auto snapshot = database->snapshot();
auto view = modb::graph::open_graph_view(*database, *snapshot);

auto adjacency = [&](ObjectId from) -> Result<std::vector<ObjectId>> {
    auto handle = database->get<Department>(from, *snapshot);
    // ... resolve a Ref<Department> "parent" field via view->outgoing_collection
    // or graph::edge(...), collecting target ObjectIds ...
};

for (auto& item : modb::graph::bfs(root_id, adjacency, {.max_depth = 5})) {
    if (!item) { return 1; }
    // process item->id, item->depth
}
```

## 7. Related Documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 12
- [reference/relationships-collections.md](relationships-collections.md) —
  `Ref`/`OwnedRef` semantics this document builds on
- [reference/queries-indexes.md](queries-indexes.md) — indexes required by
  `incoming()`, and the shared `query::Generator` laziness model
- [ADR-018](../decisions/ADR-018-handles-de-arestas-e-algoritmos-de-grafos.md)

## 8. Related Source

- `include/modb/graph/graph_view.hpp`, `edge_handle.hpp`, `traversal.hpp`,
  `algorithms.hpp`
- `examples/server/by_phase/phase_12/graph_traversal.cpp`
- `tests/graph_view_test.cpp`, `tests/graph_algorithms_test.cpp`,
  `tests/edge_handle_test.cpp`
