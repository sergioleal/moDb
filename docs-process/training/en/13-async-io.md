# Phase 13 - Asynchronous I/O

## What You Will Learn

You will submit positional asynchronous file operations and wait for an explicit
barrier.

## Related Source

`examples/server/by_phase/phase_13/async_io.cpp`

## Step by Step

The example opens an `AsyncFile` with native async I/O requested:

```cpp
modb::storage::AsyncFileOptions options;
options.max_inflight = 4;
options.require_async = true;
auto file = modb::storage::AsyncFile::open(path, Mode::create_new, options);
```

It submits operations without waiting after each one:

```cpp
file->submit_write_at(0, wal);
file->submit_sync();
file->submit_write_at(64, page);
```

Then it waits for all submitted work:

```cpp
auto drained = file->barrier();
```

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_13_async_io
.\build\debug\ring0_server_phase_13_async_io.exe
```

## Expected Output

```text
Objective: write data with positional asynchronous I/O and an explicit barrier.
backend=... max_inflight=4
```

## What To Notice

Submitting work and completing work are separate events. `barrier()` is the
point where the example requires all previously submitted operations to be done.
