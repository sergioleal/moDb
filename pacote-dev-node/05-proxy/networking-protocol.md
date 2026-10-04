# Networking and Protocol

> Part of the [`docs/reference/`](https://github.com/sergioleal/moDb/tree/v0.3.0/docs/reference) set — see [docs/README.md](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/README.md) for
> the full index and [DEVELOPER_GUIDE.md](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/DEVELOPER_GUIDE.md) for a
> narrative, tutorial-style introduction. This document is a precise
> reference: it assumes you already know C++ well and want exact behavior,
> not a learning path.

## 1. Overview

Ring0 can be used purely embedded (everything in this reference set works
without a network) or over a small, versioned binary protocol: one server
process owns a database file, and one or more client processes connect to
stream queries, call in-process domain operations, or open a facade. The
protocol layer (`modb::net`) is intentionally low-level (frames, explicit
codecs); `modb::app::ServerConnection` is the ergonomic wrapper most
application code should actually use.

The single most important invariant to internalize: **networking does not
change the transaction model.** The server enforces the same single-writer
rule from [reference/database-lifecycle.md](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/reference/database-lifecycle.md)
regardless of how many clients or concurrent streams are connected.

## 2. Concepts

### 2.1 Starting a server and connecting

```cpp
// include/modb/net/server.hpp:47-49
static Result<Server> Server::listen(const std::filesystem::path& path,
                                     std::string_view host = "127.0.0.1",
                                     std::uint16_t port = 0); // 0 = OS picks a free port
```

```cpp
// examples/server/by_phase/phase_08/connect_query.cpp (condensed)
auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
std::thread acceptor([&server] { (void)server->serve_one(); });

auto connection = modb::app::ServerConnection::connect({
    .host = "127.0.0.1",
    .port = server->port(),
    .database_name = std::string{server->database_name()},
});
auto rows = connection->collect(modb::net::QueryDescription{ .type = *item_type, .limit = 10 });
```

Note that a remote query is described by `net::QueryDescription`, keyed by a
*negotiated type id*, not a C++ template — the client may be a completely
different binary that doesn't share your `T`. `Server::serve_one()` handles
exactly one client session end to end (`Hello` through disconnect);
`serve_forever()` loops accepting sessions until `request_stop()` closes the
listener (the mechanism used to unblock `accept()` on SIGINT/SIGTERM).

### 2.2 The handshake and what gets negotiated

```cpp
// examples/server/by_phase/phase_10/handshake_capabilities.cpp (condensed)
auto info = modb::app::ServerConnection::handshake({
    .host = "127.0.0.1", .port = server->port(),
    .database_name = std::string{server->database_name()},
});
std::cout << "protocol " << info->protocol_major << '.' << info->protocol_minor
          << " max_streams=" << info->max_concurrent_streams << '\n';
```

`handshake()` performs only `Hello`/`HelloOk` and returns without sending a
query — useful for capability discovery. `Hello` carries the client's
protocol version and accepted compression codecs; `HelloOk`
(`include/modb/net/protocol.hpp:78-89`) is the server's authoritative
answer:

```cpp
struct HelloOk {
    std::uint16_t version{protocol_major};
    std::uint16_t minor{protocol_minor};
    object::BaselineId baseline{};
    Compression selected_codec{Compression::none};
    std::uint32_t max_frame_bytes{net::max_frame_bytes};
    std::uint16_t max_concurrent_streams{default_max_concurrent_streams};
    std::uint16_t max_expansion_ratio{default_max_expansion_ratio};
    std::uint32_t idle_timeout_ms{default_idle_timeout_ms};
};
```

Everything the client needs to behave correctly — frame size ceiling,
concurrency ceiling, decompression expansion ceiling, idle timeout — is
handed to it explicitly at handshake time; none of it is hardcoded
client-side.

### 2.3 Protocol limits (the defaults a server announces)

`include/modb/net/protocol.hpp:20-36`

| Constant | Value | Meaning |
|---|---:|---|
| `protocol_major` | `1` | Wire major version |
| `protocol_minor` | `0` | Additive minor; unknown minor extensions must be ignorable |
| `max_frame_bytes` | 16 MiB | A larger frame → `frame_too_large` |
| `max_string_bytes` | 64 KiB | Defensive cap on protocol strings (db name, error message) |
| `default_max_concurrent_streams` | `4` | Per-connection stream ceiling |
| `default_max_expansion_ratio` | `8` | uncompressed ≤ encoded × ratio (decompression-bomb guard) |
| `default_idle_timeout_ms` | 30000 | Idle connection timeout |
| `compression_min_bytes` | `64` | Below this, a frame isn't even attempted to be compressed |

Compression: `Compression::none` is always supported; `Compression::rle` is
negotiable and falls back to `none` if either side doesn't want it
(`is_known_compression`, `protocol.hpp:63-65`).

### 2.4 Backpressure

```cpp
// include/modb/net/server.hpp:24
inline constexpr std::size_t max_in_flight_objects = 8;
```

This is the concrete, fixed backpressure limit on the wire: the server
batches produced objects and force-flushes once a batch reaches 8 objects
(`StreamStats` on `Server` exposes `produced`/`sent`/`max_outstanding` from
the last stream for inspection/testing, `server.hpp:31-35`). There is no
unbounded queue anywhere in the streaming path.

### 2.5 Cancellation is cooperative, the connection survives it

`ServerConnection::cancel(query_id)` sends a `Cancel` frame. The server's
generator for that query stops, but it still replies with a normal
`StreamEnd` carrying the partial count — **the same connection stays usable
for a new query afterward**. This is demonstrated by the CLI's
`modb demo serve-cancel` (see [USO_DA_CLI.md](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/USO_DA_CLI.md)): cancel
mid-stream, then immediately issue a second query on the same connection.

### 2.6 Client-side concurrency model

The lower-level `net::Client` demultiplexes incoming frames by `query_id` on
a single background reader thread per connection
([`include/modb/net/client.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/net/client.hpp)). `co_await`ing a stream still resumes on the
*caller's* thread, per ADR-011 — there is no hidden thread-pool executor
scheduling your continuations onto arbitrary threads.

### 2.7 Remote access through proxies (ADR-028)

A deployment can keep the engine off the network: the engine listens only on a
local socket (`Server::open` + `listen_local`, or `local = ...` in a
`modb_add_server` config) and one or more `modb-proxy` processes face the
clients. The client protocol does not change — a client that talked to the
server talks to a proxy the same way.

```text
client ──TCP──► modb-proxy ──link (AF_UNIX, multiplexed)──► engine
```

- **Link.** One connection carries every client session of a proxy. Each frame
  is a client frame prefixed with its session number
  (`| session u32 | length u32 | type u8 | payload |`,
  [`include/modb/net/link_protocol.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/net/link_protocol.hpp)). Control messages use types ≥ `0x80`:
  `LinkHello`/`LinkHelloOk` (version, optional shared secret, and the engine's
  operation catalog), `SessionOpen` (the principal the proxy authenticated),
  `SessionClose`, `StreamCredit`.
- **Flow control.** On a shared link the TCP window cannot be the
  backpressure: the engine sends a query's `ObjectFrame`s only against
  per-stream credit, which the proxy returns as it delivers frames to the
  client. A client that stops reading stalls only its own streams.
- **Policy.** The proxy decodes every client message and runs it through a
  `modb::proxy::Policy` ([`include/modb/proxy/policy.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/proxy/policy.hpp)): `authenticate`,
  `authorize` (allow, deny, or rewrite in place), `on_response`, `audit`, and
  `on_engine` for the catalog. Reference policies: token authentication,
  allowlist, read-only, per-principal limits, audit log, composed by
  `PolicyChain` ([`include/modb/proxy/policies.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/proxy/policies.hpp)).
- **Identity.** The engine does not check credentials; it trusts the link and
  hands the session's principal, roles and attributes to procs as
  `ops::ExecutionContext::caller()` (`modb::server::Context::caller()`).
- **Authentication (protocol minor 2).** A proxy that requires it lists the
  mechanisms at the end of `HelloOk`; the client sends `Authenticate`
  (`Client::authenticate_token`, `ConnectionOptions::token`). Until then every
  request is answered with `unauthenticated`.

### 2.8 Protocol minor 3: delegation, idempotency keys, error detail (ADR-029)

Optional trailing fields, written only when set and only after minor 3 was
negotiated, so a minor 2 peer never sees them (it would reject the extra
bytes). The proxy negotiates `min(client, proxy, engine)`.

- **Delegation.** `OpCall`, `Query`, `FacadeList` and `FacadeOpen` may carry
  `acting_as` (plus up to 32 attributes): a gateway with a connection pool says
  on whose behalf each request is. Only a principal with the `delegate` role
  may send it; the proxy enforces this outside the policy chain and answers
  `permission_denied` otherwise. The engine refuses it in direct TCP mode.
  Procs see it as `ops::Caller::acting_as` / `subject()`; rate limits apply per
  delegate (`max_delegated_calls_per_second` caps the sum per principal); the
  proxy audit and the engine call log print `by <principal> as <delegate>`.
- **Idempotency key** (`OpCall` only, 1–128 bytes). In a `read_write` proc the
  engine looks the key up **inside the call's transaction** and, if the same
  principal already committed a call with it, returns the stored result
  without running the proc again; otherwise it runs the proc and stores key and
  result before the commit. The record is `sys.Idempotency` in the database
  file, so it survives a crash between commit and reply. Results over half a page (8 KB in the SDK) keep
  only the key; a repeat then gets `conflict` with
  `detail.reason = "idempotent_result_too_large"`. Enabled by
  `OperationRegistry::enable_idempotency` (the `server_host` does it;
  `idempotency_retention_s`, default 86400).
- **Error detail.** An error `OpResult` may carry `detail`, a `Value` the proc
  sent with `server::Context::fail(error, value)`; by convention `reason` and
  `field`. Sent only to sessions that negotiated minor 3.

C++: `net::CallOptions{.acting_as, .idempotency_key, .error_detail}` on
`Client::call` / `ServerConnection::call`. Covered by `modb.protocol_minor3`
(codec) and `modb.minor3_e2e` (through `server_host` and `modb-proxy`, with a
hand-rolled minor 2 client).

## 3. API Reference

### 3.1 `Server`

`include/modb/net/server.hpp:36-90`

```cpp
class Server {
public:
    static Result<Server> listen(const std::filesystem::path& path,
                                 std::string_view host = "127.0.0.1", std::uint16_t port = 0);

    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::string_view database_name() const noexcept;
    [[nodiscard]] object::BaselineId baseline() const noexcept;
    [[nodiscard]] const StreamStats& last_stream_stats() const noexcept;
    [[nodiscard]] object::Database& database() noexcept;
    [[nodiscard]] Compression selected_codec() const noexcept;

    void fail_stream_after(std::size_t objects) noexcept;       // test/fault injection
    void use_small_socket_buffers(bool enabled) noexcept;        // test/fault injection
    void set_max_concurrent_streams(std::uint16_t limit) noexcept;
    void set_idle_timeout_ms(std::uint32_t milliseconds) noexcept;
    void set_preferred_codec(Compression codec) noexcept;
    void set_operation_registry(std::shared_ptr<ops::OperationRegistry>) noexcept;
    void set_facade_catalog(std::shared_ptr<ops::FacadeCatalog>) noexcept;

    [[nodiscard]] Result<void> serve_one();
    [[nodiscard]] Result<void> serve_forever();
    void request_stop() noexcept;
    [[nodiscard]] bool stop_requested() const noexcept;
};
```

`set_operation_registry`/`set_facade_catalog` are how you wire in
[reference/domain-operations.md](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/reference/domain-operations.md) and remote facades —
without them, the server can still stream queries but has nothing to
`OpCall` into.

### 3.2 `modb::app::ServerConnection`

`include/modb/app/server_connection.hpp:24-76` — the recommended
application-facing entry point (`modb::app_client` CMake target, see
[API_PUBLICA.md](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/API_PUBLICA.md)).

```cpp
struct ConnectionOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};
    std::string database_name{};
    std::optional<std::size_t> recv_buffer_bytes{};
};

struct ServerInfo {
    std::uint16_t protocol_major{net::protocol_major};
    std::uint16_t protocol_minor{net::protocol_minor};
    object::BaselineId baseline{};
    net::Compression selected_codec{net::Compression::none};
    std::uint32_t max_frame_bytes{net::max_frame_bytes};
    std::uint16_t max_concurrent_streams{net::default_max_concurrent_streams};
    std::uint32_t idle_timeout_ms{net::default_idle_timeout_ms};
};

class ServerConnection {
public:
    static Result<ServerConnection> connect(ConnectionOptions options);
    static Result<ServerInfo> handshake(const ConnectionOptions& options);

    [[nodiscard]] const ServerInfo& info() const noexcept;
    [[nodiscard]] Result<net::ObjectStream> query(net::QueryDescription description);
    [[nodiscard]] Result<void> cancel(std::uint32_t query_id);
    [[nodiscard]] Result<std::vector<object::DecodedObject>> collect(net::QueryDescription description);
    [[nodiscard]] Result<std::vector<std::byte>> call(std::string_view operation_id,
                                                      std::span<const std::byte> args);
    [[nodiscard]] Result<std::vector<ops::FacadeDescriptor>> list_facades();
    [[nodiscard]] Result<net::FacadeOpenOk> open_facade(std::string_view facade_id, std::uint32_t version);
    template <typename TFacade>
    [[nodiscard]] Result<ops::FacadeHandle<TFacade>> open_facade();
};
```

`collect(...)` is the simple, non-streaming convenience: run a query and get
back a fully materialized `std::vector<DecodedObject>`. `query(...)` returns
a `net::ObjectStream` for incremental consumption when you don't want to
wait for the whole result set.

### 3.3 `modb::net::Client` (lower-level)

[`include/modb/net/client.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/net/client.hpp) — what `ServerConnection` wraps. Reach for
this directly only if you need protocol-level control `ServerConnection`
doesn't expose.

## 4. Semantics & Invariants

- Single-writer applies at the `Database` level regardless of connection or
  stream count — the network layer adds concurrency of *reads*/*streams*,
  not writes.
- `HelloOk`'s fields are the authoritative limits for that session; a
  client should honor `max_frame_bytes`/`max_concurrent_streams`/
  `max_expansion_ratio`/`idle_timeout_ms` as announced, not assume the
  compile-time defaults.
- A frame larger than the negotiated `max_frame_bytes` → `frame_too_large`.
- Cancelling a query ends that stream but never closes the connection —
  it remains usable for further queries/calls.
- `Compression::none` is always a valid fallback; `rle` is opportunistic.
- Frames smaller than `compression_min_bytes` are never compression
  candidates in the first place.

## 5. Common Pitfalls

- **Assuming a cancelled query closes the connection.** It doesn't — reuse
  it for the next query instead of reconnecting.
- **Ignoring the negotiated limits in `ServerInfo`/`HelloOk` and hardcoding
  your own.** The server is the source of truth for what it will accept.
- **Expecting the client's `co_await` continuations to run on a thread
  pool.** They resume on the caller's thread (ADR-011) — there is no hidden
  executor.
- **Opening more concurrent streams than `max_concurrent_streams` allows.**
  Check `ServerInfo::max_concurrent_streams` before fanning out.

## 6. Worked Example

Handshake, then a streaming query, condensed from
[`examples/server/by_phase/phase_08/connect_query.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/examples/server/by_phase/phase_08/connect_query.cpp) and
[`examples/server/by_phase/phase_10/handshake_capabilities.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/examples/server/by_phase/phase_10/handshake_capabilities.cpp):

```cpp
auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
std::thread acceptor([&server] { (void)server->serve_one(); });

auto info = modb::app::ServerConnection::handshake({
    .host = "127.0.0.1", .port = server->port(),
    .database_name = std::string{server->database_name()},
});
std::cout << "protocol " << info->protocol_major << '.' << info->protocol_minor << '\n';

auto connection = modb::app::ServerConnection::connect({
    .host = "127.0.0.1", .port = server->port(),
    .database_name = std::string{server->database_name()},
});
auto rows = connection->collect(modb::net::QueryDescription{.type = *item_type, .limit = 10});
acceptor.join();
```

## 7. Related Documentation

- [DEVELOPER_GUIDE.md](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/DEVELOPER_GUIDE.md), Chapter 9
- [reference/domain-operations.md](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/reference/domain-operations.md) — what
  `set_operation_registry`/`call` actually invoke
- [ADR-010](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/decisions/ADR-010-protocolo-binario-proximo-do-armazenamento.md),
  [ADR-011](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/decisions/ADR-011-concorrencia-do-servidor.md),
  [ADR-028](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/decisions/ADR-028-proxy-de-acesso-remoto.md) (proxies),
  [ADR-029](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/decisions/ADR-029-delegacao-detalhe-e-idempotencia.md) (minor 3)
- [OPERACAO.md](https://github.com/sergioleal/moDb/blob/v0.3.0/docs/OPERACAO.md), "Proxy de acesso remoto" — running `modb-proxy`

## 8. Related Source

- [`include/modb/net/server.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/net/server.hpp), `client.hpp`, `protocol.hpp`,
  `native_socket.hpp`, `query_description.hpp`
- [`include/modb/app/server_connection.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/app/server_connection.hpp)
- [`include/modb/net/link_protocol.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/include/modb/net/link_protocol.hpp), [`src/net/engine_link.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/src/net/engine_link.cpp),
  [`src/net/engine_session.hpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/src/net/engine_session.hpp) (the engine side of the link)
- [`include/modb/proxy/`](https://github.com/sergioleal/moDb/tree/v0.3.0/include/modb/proxy) (`proxy.hpp`, `policy.hpp`, `policies.hpp`,
  `token_policy.hpp`), [`apps/modb_proxy/main.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/apps/modb_proxy/main.cpp)
- [`examples/server/by_phase/phase_08/connect_query.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/examples/server/by_phase/phase_08/connect_query.cpp),
  [`examples/server/by_phase/phase_10/handshake_capabilities.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/examples/server/by_phase/phase_10/handshake_capabilities.cpp)
- [`tests/server_streaming_test.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/tests/server_streaming_test.cpp), [`tests/protocol_test.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/tests/protocol_test.cpp),
  [`tests/app_server_connection_test.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/tests/app_server_connection_test.cpp), [`tests/protocol_minor3_test.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/tests/protocol_minor3_test.cpp),
  [`tests/minor3_e2e_test.cpp`](https://github.com/sergioleal/moDb/blob/v0.3.0/tests/minor3_e2e_test.cpp)
