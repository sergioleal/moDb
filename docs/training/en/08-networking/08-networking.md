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
split.

## New Concepts

- `Server::listen`/`serve_one`/`serve_forever`, `ServerConnection::connect`/
  `handshake`, `QueryDescription` — see
  [docs/reference/networking-protocol.md](../../../reference/networking-protocol.md).
- Protocol limits, backpressure (`max_in_flight_objects`), cooperative
  cancellation and connection reuse.

## Starting Point

The persistent database file after Lessons 1-7 have run, with an indexed
salary search already in place.

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

[lesson_08_networking.cpp](lesson_08_networking.cpp)

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
Handshake ok, protocol 1.0, max_concurrent_streams=4
Remote query returned 3 employees
Remote search for salary == 20000 returned 1 match(es): Carla
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

- [docs/reference/networking-protocol.md](../../../reference/networking-protocol.md)
- [DEVELOPER_GUIDE.md, Chapter 9](../../../DEVELOPER_GUIDE.md#9-server-and-client)
