# Phase 08 - Server Connection

## What You Will Learn

You will start a loopback server and use `ServerConnection` to collect remote
query results.

## Related Source

`examples/server/by_phase/phase_08/connect_query.cpp`

## Step by Step

The example seeds a database locally, then opens it through a server:

```cpp
auto server = modb::net::Server::listen(path, "127.0.0.1", 0);
```

Port `0` asks the OS to choose an available port. The example then serves one
session in a background thread:

```cpp
std::thread acceptor([&server] { (void)server->serve_one(); });
```

The application side uses the small client library:

```cpp
auto connection = modb::app::ServerConnection::connect(...);
auto rows = connection->collect(query);
```

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_08_connect_query
.\build\debug\ring0_server_phase_08_connect_query.exe
```

## Expected Output

```text
Objective: connect to a Ring0 server and collect objects with ServerConnection.
connected protocol 1.0
objects received: 2
```

## What To Notice

The query sent over the wire uses `QueryDescription` and a type id. The remote
client is not executing a C++ lambda inside the server.
