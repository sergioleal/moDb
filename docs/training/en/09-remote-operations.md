# Lesson 9 — Remote Operations: Transferring an Employee

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

A `department.transfer` operation that runs **on the server**, callable by
id from the client: move an employee to a new department, validating the
target department exists before making any change, all inside the server's
own transaction.

## New Concepts

- The `Operation` contract, `OperationRegistry::register_operation`/
  `dispatch`, `ExecutionContext`, `ModuleManifest` + hash allowlist — see
  [docs/reference/domain-operations.md](../../reference/domain-operations.md).

## Starting Point

Lesson 8's server/client split.

## Steps

- TODO: define `TransferDepartment : Operation` (`k_id = "department.transfer"`,
  `encode_args`/`decode` for `employee_id` + `new_department_id`).
- TODO: `execute()` validates the target department exists (fails cleanly if
  not) before mutating the employee's `department` field.
- TODO: build a `ModuleManifest`, compute its hash, `admit_hash`, `load` it
  into an `OperationRegistry`, and `server->set_operation_registry(...)`.
- TODO: client calls it with `connection->call(TransferDepartment::k_id, *args)`
  and prints the result.
- TODO: demonstrate the rollback path: call with a nonexistent department
  id and show the employee's department is unchanged afterward.

## Full Listing (End of Lesson)

TODO — target: extends `examples/employee_directory/lesson_08_server.cpp`/
`lesson_08_client.cpp` into `lesson_09_*`.

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_09_server employee_directory_lesson_09_client
.\build\debug\employee_directory_lesson_09_server.exe
.\build\debug\employee_directory_lesson_09_client.exe 127.0.0.1 <port>
```

## Expected Output

TODO — a successful transfer, and a rejected one (bad department id) that
leaves the employee untouched.

## What to Notice

- TODO: validate before mutating — a `Result` failure rolls back the
  transaction, not any state you might have changed outside it first.
- TODO: the client never links against `TransferDepartment`'s
  implementation — only its id and argument encoding.

## Related Reference

- [docs/reference/domain-operations.md](../../reference/domain-operations.md)
- [OPERACAO_MODULOS.md](../../OPERACAO_MODULOS.md)
- [DEVELOPER_GUIDE.md, Chapter 10](../../DEVELOPER_GUIDE.md#10-remote-domain-operations)
