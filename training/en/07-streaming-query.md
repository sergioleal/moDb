# Phase 07 - Streaming Queries

## What You Will Learn

You will stream typed query results with a filter and a limit.

## Related Source

`examples/server/by_phase/phase_07/streaming_query.cpp`

## Step by Step

The example seeds six `Item` objects and streams only the first two even values:

```cpp
database->query<Item>()
    .where([](const Item& item) { return item.value % 2 == 0; })
    .limit(2)
    .stream()
```

For a C++ developer, the API reads like a lazy range. The important database
idea is that results are delivered one at a time.

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_07_streaming_query
.\build\debug\ring0_server_phase_07_streaming_query.exe
```

## Expected Output

```text
Objective: run a typed streaming query with a filter and a limit.
item-0
item-2
```

## What To Notice

The caller checks each streamed `Result`. Streaming can report errors after some
rows have already been delivered.
