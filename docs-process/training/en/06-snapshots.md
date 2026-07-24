# Phase 06 - Snapshots

## What You Will Learn

You will keep a stable read view while a later transaction updates the object.

## Related Source

`examples/server/by_phase/phase_06/snapshot_read.cpp`

## Step by Step

The example commits an account with balance `100`, opens a snapshot, then commits
a second transaction that changes the balance to `200`.

```cpp
auto snapshot = database->snapshot();
...
handle->set<&Account::balance>(*tx2, 200);
tx2->commit();
```

The snapshot read and the current read intentionally disagree:

```cpp
auto stable = database->get<Account>(account->id(), *snapshot);
auto current = database->materialize(*database->get<Account>(account->id()));
```

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_06_snapshot_read
.\build\debug\ring0_server_phase_06_snapshot_read.exe
```

## Expected Output

```text
Objective: prove that a snapshot keeps a stable view after a later commit.
snapshot=100 current=200
```

## What To Notice

A snapshot is not a copy of the whole database. It is a read view over object
versions.
