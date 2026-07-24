# Phase 12 - Graph Traversal

## What You Will Learn

You will traverse a graph with breadth-first search using an adjacency callback.

## Related Source

`examples/server/by_phase/phase_12/graph_traversal.cpp`

## Step by Step

The traversal engine does not require a specific container. It needs a starting
`ObjectId` and a callback that returns outgoing neighbors:

```cpp
auto adjacency = [&edges](ObjectId from) -> modb::Result<std::vector<ObjectId>> {
    return edges[from.value];
};
```

The BFS result is streamed:

```cpp
for (auto& item : modb::graph::bfs(ObjectId{1}, adjacency)) {
    ...
}
```

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_12_graph_traversal
.\build\debug\ring0_server_phase_12_graph_traversal.exe
```

## Expected Output

```text
Objective: traverse a graph with breadth-first search.
visit 1 depth=0
visit 2 depth=1
visit 3 depth=1
visit 4 depth=2
```

## What To Notice

The example uses an in-memory map so the traversal contract is obvious. In a
database-backed application, the adjacency callback would resolve stored edges.
