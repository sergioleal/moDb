# Phase 02 - Persist and Reopen

## What You Will Learn

You will create an object, commit it, reopen the database, and read it back by
`ObjectId`.

## Related Source

`examples/server/by_phase/phase_02/persist_reopen.cpp`

## Step by Step

The example reuses the `Customer` binding from phase 01. It then opens a
transaction and creates one object:

```cpp
auto tx = database->begin();
auto customer = database->create(*tx, Customer{"Ana", 42});
tx->commit();
```

After the first database lifetime ends, the file is reopened:

```cpp
auto opened = modb::object::Database::open(path);
```

The binding is registered again before materialization. This is normal for typed
C++ APIs: the stored object has bytes and type metadata, but the process still
needs the C++ binding to decode those bytes into `Customer`.

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_02_persist_reopen
.\build\debug\ring0_server_phase_02_persist_reopen.exe
```

## Expected Output

```text
Objective: persist an object, reopen the database, and read it by ObjectId.
Ana score=42
```

## What To Notice

`ObjectId` is the stable identity. You do not search by physical page or slot;
you ask the object layer for the logical object.
