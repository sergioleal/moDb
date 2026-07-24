# Ring0 Server Training

This training teaches Ring0 through the example applications under
`examples/server/by_phase`. It assumes you are comfortable with modern C++:
structs, templates, lambdas, RAII, smart pointers, `std::filesystem`, and
ordinary build tools.

The course is incremental. Each lesson introduces one new concept and reuses
ideas from the previous lessons.

## Lessons

1. [Phase 00 - Version and Compatibility](00-version-compatibility.md)
2. [Phase 01 - Binding a C++ Type](01-bind-type.md)
3. [Phase 02 - Persist and Reopen](02-persist-reopen.md)
4. [Phase 03 - Updating with Handle<T>](03-handle-update.md)
5. [Phase 04 - Relationships](04-relationships.md)
6. [Phase 05 - Transactions and Recovery](05-transactions-recovery.md)
7. [Phase 06 - Snapshots](06-snapshots.md)
8. [Phase 07 - Streaming Queries](07-streaming-query.md)
9. [Phase 08 - Server Connection](08-server-connection.md)
10. [Phase 09 - Remote Operation](09-remote-operation.md)
11. [Phase 10 - Handshake Capabilities](10-handshake-capabilities.md)
12. [Phase 11 - Remote Facade](11-remote-facade.md)
13. [Phase 12 - Graph Traversal](12-graph-traversal.md)
14. [Phase 13 - Asynchronous I/O](13-async-io.md)

## Build the Examples

```powershell
cmake --preset debug
cmake --build --preset debug --target ring0_server_phase_00_version_compatibility
```

Every lesson names the exact target and executable for that phase.
