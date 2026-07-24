# Phase 05 - Transactions and Recovery

## What You Will Learn

You will commit durable data and verify it after a real reopen.

## Related Source

`examples/server/by_phase/phase_05/transaction_recovery.cpp`

## Step by Step

The example writes an `Account` in one database lifetime, commits, closes that
lifetime, then opens the file again.

That structure matters:

```cpp
{
    auto created = modb::object::Database::create(path);
    ...
    tx->commit();
}

auto opened = modb::object::Database::open(path);
```

The reopen validates the durable path rather than only checking in-memory state.

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_05_transaction_recovery
.\build\debug\ring0_server_phase_05_transaction_recovery.exe
```

## Expected Output

```text
Objective: commit durable data and verify it after reopening the database.
Alice recovered balance=100
```

## What To Notice

The example is intentionally simple. Recovery becomes more interesting under
crash failpoints, but the application-level contract starts here: committed data
survives reopen.
