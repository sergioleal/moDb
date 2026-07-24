# Phase 01 - Binding a C++ Type

## What You Will Learn

You will map a normal C++ struct into the Ring0 catalog.

## Related Source

`examples/server/by_phase/phase_01/bind_type.cpp`

## Step by Step

The domain type is deliberately plain:

```cpp
struct Customer {
    std::string name;
    std::int64_t score{};
};
```

The binding names the persistent type and maps stable field ids to C++ members:

```cpp
modb::object::BindingBuilder<Customer> builder{"Customer"};
builder.field<1>("name", &Customer::name)
       .field<2>("score", &Customer::score);
```

The key idea is that field ids are the durable contract. Field names are useful
for humans, but the ids are what let Ring0 decode stored objects predictably.

## Build and Run

```powershell
cmake --build build/debug --target ring0_server_phase_01_bind_type
.\build\debug\ring0_server_phase_01_bind_type.exe
```

## Expected Output

```text
Objective: bind a C++ type and register it in the catalog.
Customer type id: ...
```

## What To Notice

The example creates a temporary database only to prove that the type reaches the
catalog. It does not store a `Customer` object yet. That comes next.
