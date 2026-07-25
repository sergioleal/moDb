# Lesson 18 -- Protocol and Compatibility

> **Status:** code written; run after Lesson 17.

## What You'll Add

A compatibility check that mirrors the habits used by the network handshake:
encode and decode major/minor versions, negotiate a protocol version, and show
the two failure modes that must stop a client before it talks to a server.

## New Concepts

- `CompatibilityVersion`, `to_wire_u16`, and `from_wire_u16`.
- `ensure_readable` for persisted artifacts.
- `negotiate_protocol_version` for client/server protocol selection.

## Starting Point

Lesson 17 explained why catalog baselines name a shape. This lesson explains
the adjacent question: how two processes agree that they can even understand
the same bytes.

## Steps

- Encode `{major=1, minor=2}` to the compact wire representation and decode it
  back.
- Negotiate a client at protocol `1.4` with a server at `1.2`; the effective
  version is `1.2`.
- Demonstrate a minor-version read failure and a major-version protocol
  failure without opening a socket.

## Full Listing (End of Lesson)

[lesson_18_protocol_compatibility.cpp](lesson_18_protocol_compatibility.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_18
.\build\debug\employee_directory_lesson_18.exe
```

## Expected Output

```
Objective: practice compatibility before crossing a process boundary.
Wire value for 1.2: 258
Decoded wire value: 1.2
Negotiated protocol: 1.2
Readable check for artifact 1.3 with reader 1.2: rejected
Protocol negotiation with major mismatch: rejected
```

## What to Notice

- Minor versions are additive; major versions are compatibility boundaries.
- Artifact readability and peer negotiation are related but not identical
  checks.
- A clean rejection before data exchange is much cheaper than discovering an
  incompatible frame halfway through a stream.

## Next

Continue with [Lesson 19 -- Replication Catch-up and wal_only](../19-replication-catchup-walonly/19-replication-catchup-walonly.md).

## Related Reference

- [COMPATIBILIDADE.md](../../../COMPATIBILIDADE.md)
- [docs/reference/networking-protocol.md](../../../reference/networking-protocol.md)
- [USO_DA_CLI.md](../../../USO_DA_CLI.md) -- `demo protocol`
