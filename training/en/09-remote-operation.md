# Phase 09 - Remote Operation

## What You Will Learn

You will call a domain operation over the server.

## Related Source

`examples/server/by_phase/phase_09/call_operation.cpp`

## Step by Step

The example seeds two accounts and registers the `TransferFunds` operation in an
`OperationRegistry`.

```cpp
auto registry = std::make_shared<modb::ops::OperationRegistry>();
server->set_operation_registry(registry);
```

The module manifest is admitted before loading:

```cpp
loader.admit_hash(manifest.hash);
loader.load(manifest, baseline, *registry, ...);
```

The client encodes operation arguments and calls by operation id:

```cpp
auto args = modb::examples::TransferFunds::encode_args(alice, bob, 40);
result = connection->call(modb::examples::TransferFunds::k_id, *args);
```

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_09_call_operation
.\build\debug\ring0_server_phase_09_call_operation.exe
```

## Expected Output

```text
Objective: call a remote domain operation through the Ring0 server.
remote TransferFunds committed
```

## What To Notice

The server owns the transaction boundary for the operation. The client sends
intent and arguments; it does not mutate server storage directly.
