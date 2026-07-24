# Phase 10 - Handshake Capabilities

## What You Will Learn

You will inspect the public capabilities negotiated during a server handshake.

## Related Source

`examples/server/by_phase/phase_10/handshake_capabilities.cpp`

## Step by Step

This example creates an empty database, starts a server, and performs only the
handshake:

```cpp
auto info = modb::app::ServerConnection::handshake(...);
```

The returned info includes protocol version and server limits:

```cpp
info->protocol_major
info->protocol_minor
info->max_concurrent_streams
```

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_10_handshake_capabilities
.\build\debug\ring0_server_phase_10_handshake_capabilities.exe
```

## Expected Output

```text
Objective: inspect public server capabilities negotiated during handshake.
protocol 1.0 max_streams=...
```

## What To Notice

Capability discovery is useful before choosing client behavior. It is the
network version of the compatibility habit you saw in phase 00.
