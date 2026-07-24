# Phase 03 - Updating with Handle<T>

## What You Will Learn

You will update one field through a typed `Handle<T>` inside a transaction.

## Related Source

`examples/server/by_phase/phase_03/handle_update.cpp`

## Step by Step

The example creates an `Account`:

```cpp
auto account = database->create(*tx, Account{"Alice", 100});
```

Then it updates the `balance` member through the handle:

```cpp
account->set<&Account::balance>(*tx, 125);
```

This is idiomatic Ring0 C++: the member pointer identifies the persistent field
without string lookup at the call site.

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_03_handle_update
.\build\debug\ring0_server_phase_03_handle_update.exe
```

## Expected Output

```text
Objective: update a typed object through Handle<T> inside a transaction.
Alice balance=125
```

## What To Notice

The update is not durable until the transaction commits. Treat `Handle<T>` as a
typed object reference, not as permission to bypass transactions.
