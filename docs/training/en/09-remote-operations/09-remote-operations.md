# Lesson 9 — Remote Operations: Transferring an Employee

> **Status:** ✅ code written and verified against a real build.

## What You'll Add

A `department.transfer` operation that runs **on the server**, callable by
id from the client: move an employee to a new department, validating the
target department exists before making any change, all inside the server's
own transaction.

## New Concepts

- The `Operation` contract, `OperationRegistry::register_operation`/
  `dispatch`, `ExecutionContext`, `ModuleManifest` + hash allowlist — see
  [docs/reference/domain-operations.md](../../../reference/domain-operations.md).

## Starting Point

The persistent database file after Lessons 1-8 have run.

## Steps

- Look Bruno up by name, and "Engineering" up by name through Lesson 5's
  `Department.name` index.
- Define `TransferDepartment : Operation` (`k_id = "department.transfer"`,
  `encode_args`/`decode` for `employee_id` + `department_id`, both `u64`s).
- `execute()` validates the target department exists (via
  `ExecutionContext::objects().get<Department>(...)`) and fails cleanly if
  not, *before* materializing or mutating the employee.
- Build a `ModuleManifest` (id, module version, baseline, exported
  methods), compute its hash with `compute_manifest_hash`, `admit_hash` it
  on a `ModuleLoader`, `load` it into an `OperationRegistry`, and call
  `server->set_operation_registry(registry)`.
- Client calls it with `connection->call(TransferDepartment::k_id,
  *encoded_args)` to move Bruno back to Engineering (his old department,
  Sales, was removed back in Lesson 5 — this call also cleans up that
  dangling reference).
- Demonstrate the rejection path: call with a nonexistent department id
  (`ObjectId{999999}`) and confirm it fails with "target department does
  not exist" rather than partially mutating the employee.

## Full Listing (End of Lesson)

[lesson_09_remote_operations.cpp](lesson_09_remote_operations.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_09
.\build\debug\employee_directory_lesson_09.exe
```

## Expected Output

```
Objective: call a server-side domain operation from a remote client.
Transferring Bruno to Engineering (his old department, Sales, was removed back in Lesson 5)
  succeeded
Attempting a transfer to a department that doesn't exist
  failed as expected: target department does not exist
```

## What to Notice

- Validate before mutating — a `Result` failure returned from `execute()`
  rolls back the transaction, so the failed transfer above leaves Bruno's
  `department` field exactly as it was.
- The client never links against `TransferDepartment`'s implementation —
  only its id string and `encode_args`. The class itself, including its
  validation logic, lives and runs entirely on the server.

## Related Reference

- [docs/reference/domain-operations.md](../../../reference/domain-operations.md)
- [OPERACAO_MODULOS.md](../../../OPERACAO_MODULOS.md)
- [DEVELOPER_GUIDE.md, Chapter 10](../../../DEVELOPER_GUIDE.md#10-remote-domain-operations)
