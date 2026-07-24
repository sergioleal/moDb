# Phase 04 - Relationships

## What You Will Learn

You will store object relationships with `Ref<T>` and `OwnedRef<T>`.

## Related Source

`examples/server/by_phase/phase_04/relationships.cpp`

## Step by Step

The example has three domain types:

```cpp
struct Department;
struct Badge;
struct Employee;
```

`Employee` points to a `Department` by association and owns a `Badge`:

```cpp
modb::object::Ref<Department> department{};
modb::object::OwnedRef<Badge> badge{};
```

Both relationships are stored as logical object identities. The difference is
semantic: `Ref<T>` is an association, while `OwnedRef<T>` represents ownership.

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_04_relationships
.\build\debug\ring0_server_phase_04_relationships.exe
```

## Expected Output

```text
Objective: store domain relationships with Ref<T> and OwnedRef<T>.
Ana department_ref=... badge_owned_ref=...
```

## What To Notice

The example prints ids, not nested objects. A relationship is a durable edge; the
application can resolve it when it needs the target object.
