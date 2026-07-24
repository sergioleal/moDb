# Lesson 8 — Serving the Directory Over the Network

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

You split the single program into two: a **server** that owns the directory
file and a separate **client** that connects over TCP and runs the "search"
queries from Lesson 7 remotely, plus a capability handshake that prints
what the server negotiated.

## New Concepts

- `Server::listen`/`serve_one`/`serve_forever`, `ServerConnection::connect`/
  `handshake`, `QueryDescription` — see
  [docs/reference/networking-protocol.md](../../reference/networking-protocol.md).
- Protocol limits, backpressure (`max_in_flight_objects`), cooperative
  cancellation and connection reuse.

## Starting Point

Lesson 7's directory with an indexed salary search.

## Steps

- TODO: `employee_directory_server`: `Server::listen(path, "127.0.0.1", 0)`,
  print the chosen port, `serve_forever()` until Ctrl+C.
- TODO: `employee_directory_client`: `ServerConnection::handshake(...)` to
  print negotiated protocol/limits, then `ServerConnection::connect(...)`.
- TODO: port the "employees above salary N" query to run remotely via
  `connection->collect(QueryDescription{...})`.
- TODO: start a long-running query, `cancel()` it partway through, and issue
  a second query on the same connection to show it's still usable.

## Full Listing (End of Lesson)

TODO — targets: `examples/employee_directory/lesson_08_server.cpp`,
`examples/employee_directory/lesson_08_client.cpp`.

## Build and Run

TODO — two processes:

```powershell
cmake --build --preset debug --target employee_directory_lesson_08_server employee_directory_lesson_08_client
.\build\debug\employee_directory_lesson_08_server.exe
# in a second terminal:
.\build\debug\employee_directory_lesson_08_client.exe 127.0.0.1 <port>
```

## Expected Output

TODO — handshake info printed by the client, remote search results
matching Lesson 7's local results, and a cancel-then-reuse demonstration.

## What to Notice

- TODO: single-writer still applies — the network doesn't add concurrent
  writers, only concurrent readers/streams.
- TODO: cancelling a query never closes the connection.

## Related Reference

- [docs/reference/networking-protocol.md](../../reference/networking-protocol.md)
- [DEVELOPER_GUIDE.md, Chapter 9](../../DEVELOPER_GUIDE.md#9-server-and-client)
