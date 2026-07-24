# Phase 11 - Remote Facade

## What You Will Learn

You will open a typed remote facade and invoke a method through it.

## Related Source

`examples/server/by_phase/phase_11/open_facade.cpp`

## Step by Step

Phase 09 called an operation by id. Phase 11 adds a typed facade on top of that
operation catalog.

The server registers both pieces:

```cpp
server->set_operation_registry(registry);
server->set_facade_catalog(catalog);
```

The client asks for a typed facade:

```cpp
auto handle = connection->open_facade<modb::examples::AccountsFacade>();
```

Then it invokes a typed method:

```cpp
result = handle->invoke<modb::examples::TransferFunds>(alice, bob, 25);
```

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_11_open_facade
.\build\debug\ring0_server_phase_11_open_facade.exe
```

## Expected Output

```text
Objective: open a typed remote facade and invoke it through the server.
remote accounts facade invoked
```

## What To Notice

Facades improve the consumer API without replacing operations. Under the hood,
the invocation still travels as a checked remote operation call.
