# Phase 00 - Version and Compatibility

## What You Will Learn

You will start with the smallest public contract: project version and protocol
compatibility. No database file is opened in this phase.

## Related Source

`examples/server/by_phase/phase_00/version_compatibility.cpp`

## Step by Step

The example creates two `CompatibilityVersion` values: one for the client and
one for the server.

```cpp
const modb::CompatibilityVersion client{1, 0};
const modb::CompatibilityVersion server{1, 0};
auto negotiated = modb::negotiate_protocol_version(client, server);
```

For a C++ developer, this should feel like a normal capability check. The
important Ring0 idea is that clients should not assume that a server speaks the
same protocol version. They negotiate first, then decide whether to continue.

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_00_version_compatibility
.\build\debug\ring0_server_phase_00_version_compatibility.exe
```

## Expected Output

```text
Objective: show project version and protocol compatibility negotiation.
moDb 0.1.0
protocol 1.0
```

## What To Notice

This phase does not depend on storage, object bindings, or networking. It is the
front door for all later examples.
