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
  [docs/reference/domain-operations.md](../../reference/domain-operations.md).

## Starting Point

Lesson 8's server/client, still in one process.

## Steps

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

[examples/employee_directory/lesson_09_remote_operations.cpp](../../../examples/employee_directory/lesson_09_remote_operations.cpp)

## Build and Run

```powershell
cmake --build --preset debug --target employee_directory_lesson_09
.\build\debug\employee_directory_lesson_09.exe
```

## Expected Output

```
Objective: call a server-side domain operation from a remote client.
Lesson 1: Employee type id = 16
Lesson 2: wrote 3 employees (Ana=18, Bruno=19, Carla=20)
Lesson 2: after reopen, employee 18 = Ana (12000)
Lesson 3: committed raise for Bruno
  Bruno after committed raise: Bruno = 10500
Lesson 3: uncommitted raise for Carla (deliberately not committed)
  Carla after scope exit (should be unchanged): Carla = 15000
Lesson 3: averaging Ana and Bruno's salaries in one transaction
  Ana after averaging: Ana = 11250
  Bruno after averaging: Bruno = 11250
Lesson 3: attempting a second transaction while one is open
  second begin() failed as expected: a transaction is already in progress
Lesson 4: Ana read through the new binding = Ana (11250, country=BR) -- country came from the declared default, not from disk
Lesson 4: raise for Ana via Handle::set (not manual materialize/update)
  Ana after Handle::set raise: Ana (13250, country=BR) -- now physically stored in the new 3-field shape
Lesson 5: created departments Engineering=31, Sales=32
Lesson 5: assigned Ana -> Engineering, Bruno -> Sales
Lesson 5: Diego=34 has emergency contact 33
Lesson 5: after removing Diego, his emergency contact is gone too (cascade-deleted): no object with id 33
Lesson 5: Carla's projects: Phoenix Atlas
Lesson 5: after removing Sales, resolving it directly fails as expected: no object with id 32 -- Bruno's `department` field still holds that (now dangling) id
Lesson 6: payroll total at the snapshot's epoch = 39500
Lesson 6: payroll total at the SAME snapshot after Carla's raise = 39500 (unchanged -- the snapshot doesn't see it)
Lesson 6: second raise while the snapshot is open failed as expected: object 20 already has a previous version visible to an older open snapshot
Lesson 6: retried raise for Carla succeeded once the snapshot closed
Lesson 6: payroll total on a fresh snapshot = 44500
Lesson 6: collect_garbage() reclaimed 12 record(s)
  employees earning >= 15000 (no index yet): access=table_scan index_available=false
  results: Carla
Lesson 7: created an index on Employee.salary
  employees earning >= 15000 (indexed): access=index_scan index_available=true
  results: Carla
Lesson 7: top 2 earners: Carla(20000) Ana(13250)
Lesson 8: handshake ok, protocol 1.0, max_concurrent_streams=4
Lesson 8: remote query returned 3 employees
Lesson 8: remote search for salary == 20000 returned 1 match(es): Carla
Lesson 9: transferring Bruno to Engineering (his old department, Sales, was removed back in Lesson 5)
  succeeded
Lesson 9: attempting a transfer to a department that doesn't exist
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

- [docs/reference/domain-operations.md](../../reference/domain-operations.md)
- [OPERACAO_MODULOS.md](../../OPERACAO_MODULOS.md)
- [DEVELOPER_GUIDE.md, Chapter 10](../../DEVELOPER_GUIDE.md#10-remote-domain-operations)
