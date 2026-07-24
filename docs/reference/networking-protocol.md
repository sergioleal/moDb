# Networking and Protocol

> **Status:** 🚧 skeleton — outline only, not yet written. In the meantime see
> [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 9, and
> [streaming.md](../../streaming.md) for the vision-level rationale.

## Overview

- TODO: server/client roles, one database file per server, versioned binary
  framing (`Hello`/`HelloOk` handshake).
- TODO: single-writer at the `Database` level regardless of how many clients
  or streams are connected.

## Core API surface

- TODO `Server::listen(path, host, port)` / `serve_one()` / `serve_forever()`
  / `request_stop()` — `include/modb/net/server.hpp:47-86`
- TODO `ServerConnection::connect(ConnectionOptions)` /
  `ServerConnection::handshake(...)` — `include/modb/app/server_connection.hpp:50-51`
- TODO `ServerConnection::query/cancel/collect/call` — `server_connection.hpp:55-61`
- TODO lower-level `net::Client::connect/query/cancel/call` —
  `include/modb/net/client.hpp:112-127`
- TODO `HelloOk` fields (negotiated version, codec, limits) —
  `include/modb/net/protocol.hpp:78-89`
- TODO `net::QueryDescription` — `include/modb/net/query_description.hpp`

## Semantics & invariants

- TODO: protocol limits — `include/modb/net/protocol.hpp:23-38`
  (`max_frame_bytes=16MiB`, `max_string_bytes=64KiB`,
  `default_max_concurrent_streams=4`, `default_max_expansion_ratio=8`,
  `default_idle_timeout_ms=30000`, `compression_min_bytes=64`).
- TODO: backpressure — server force-flushes at `max_in_flight_objects=8`
  (`net/server.hpp:25`).
- TODO: cancellation is cooperative (`Cancel{query_id}`); the connection
  stays reusable afterward.
- TODO: client-side demultiplexing by `query_id` on a single reader thread
  per connection; `co_await` executes on the caller's thread (ADR-011, no
  thread-pool executor).

## Common pitfalls

- TODO: assuming a cancelled stream closes the connection.
- TODO: exceeding `max_concurrent_streams` on one connection.

## Related documentation

- [DEVELOPER_GUIDE.md](../DEVELOPER_GUIDE.md), Chapter 9
- [ADR-010](../decisions/ADR-010-protocolo-binario-proximo-do-armazenamento.md),
  [ADR-011](../decisions/ADR-011-concorrencia-do-servidor.md)

## Related source

- `include/modb/net/server.hpp`, `client.hpp`, `protocol.hpp`,
  `replication_protocol.hpp`, `native_socket.hpp`
- `include/modb/app/server_connection.hpp`
- `examples/server/by_phase/phase_08/connect_query.cpp`,
  `phase_10/handshake_capabilities.cpp`
- `tests/server_streaming_tests.cpp`, `tests/protocol_tests.cpp`,
  `tests/app_server_connection_test.cpp`
