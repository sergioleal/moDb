# Lesson 8 — Serving the Directory Over the Network

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

The directory starts serving itself over TCP: one process opens a
`Server`, binds `Employee`, and accepts a single connection on a
background thread, while the main thread acts as the client — connecting,
running a capability handshake, and re-running the "search" queries from
Lesson 7 *remotely* via `QueryDescription` instead of the local `Query<T>`
builder.

This lesson keeps server and client in **one binary** (a background
thread runs `serve_one()` while the main thread drives the connection) —
the same single-process convention every later networking lesson
(9 and 10) also uses. A real deployment would split them into separate
processes, but nothing about `Server`/`ServerConnection` requires that
split, and keeping them together here keeps the cumulative-file story
intact.

## New Concepts

- `Server::listen`/`serve_one`/`serve_forever`, `ServerConnection::connect`/
  `handshake`, `QueryDescription` — see
  [docs/reference/networking-protocol.md](../../reference/networking-protocol.md).
- Protocol limits, backpressure (`max_in_flight_objects`), cooperative
  cancellation and connection reuse.

## Starting Point

Lesson 7's directory with an indexed salary search.

## Steps

- `Server::listen(path, "127.0.0.1", 0)` (port `0` = "pick any free port"),
  bind `Employee`, and start a background `std::thread` running
  `server->serve_one()` (accepts exactly one connection, then returns).
- `ServerConnection::connect({.host, .port, .database_name})` from the main
  thread; print the negotiated protocol version and
  `max_concurrent_streams` from `connection->info()`.
- Run `connection->collect(QueryDescription{.type = employee_type, .limit
  = 10})` to fetch every employee remotely.
- Run a second `collect()` with `QueryDescription{.equals =
  EqualityFilter{salary_field, AttributeValue{20000}}}` to find one
  employee by exact salary match — this is the wire protocol's search
  primitive; unlike the local `Query<T>` builder, it only supports
  equality, not `.between()`.

## Full Listing (End of Lesson)

[examples/employee_directory/lesson_08_networking.cpp](../../../examples/employee_directory/lesson_08_networking.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_08
.\build\debug\employee_directory_lesson_08.exe
```

The handshake and the network round-trip can take longer than a purely
local lesson on a first run — give it a few seconds.

## Expected Output

```
Objective: serve the directory and query it remotely with ServerConnection.
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
```

## What to Notice

- Single-writer still applies — the network doesn't add concurrent
  writers, only a connection through which one client streams queries.
- `QueryDescription` is deliberately narrower than the local `Query<T>`
  builder: it carries a type, an optional `limit`, and an optional single
  `EqualityFilter` — no `.between()`, no `top_k`, no composition. If you
  need those over the wire, that's what Lesson 9's domain operations and
  Lesson 10's facades are for.

## Related Reference

- [docs/reference/networking-protocol.md](../../reference/networking-protocol.md)
- [DEVELOPER_GUIDE.md, Chapter 9](../../DEVELOPER_GUIDE.md#9-server-and-client)
