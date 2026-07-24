# Lesson 10 — Facades: A Stable HR API

> **Status:** 🚧 skeleton — structure and goals only, step-by-step content
> and code not yet written.

## What You'll Add

An `HRFacade` grouping `TransferDepartment` (from Lesson 9) and a new
`GiveRaise` operation behind one versioned, typed surface. The client opens
the facade once and calls both methods through it, without knowing the
server's concrete operation classes.

## New Concepts

- `FacadeHandle<TFacade>::invoke<Method>`, `FacadeCatalog`, id+version
  identity (not array index), embedded vs. remote invoker — see
  [FACADES.md](../../FACADES.md).

## Starting Point

Lesson 9's server with `TransferDepartment` registered.

## Steps

- TODO: add a `GiveRaise` operation (employee id + new salary), same shape
  as `TransferDepartment`.
- TODO: define the `HRFacade` tag type and its `FacadeDescriptor` (id,
  version, exported methods).
- TODO: wire the facade into the manifest's `facades` field and register it
  in a `FacadeCatalog` server-side.
- TODO: client: `connection->open_facade<HRFacade>()`, then
  `handle->invoke<TransferDepartment>(...)` and `handle->invoke<GiveRaise>(...)`.
- TODO: demonstrate a version mismatch: open the facade asking for a wrong
  version and show `incompatible_facade_version`.

## Full Listing (End of Lesson)

TODO — extends the Lesson 9 server/client into `lesson_10_*`.

## Build and Run

TODO

```powershell
cmake --build --preset debug --target employee_directory_lesson_10_server employee_directory_lesson_10_client
.\build\debug\employee_directory_lesson_10_server.exe
.\build\debug\employee_directory_lesson_10_client.exe 127.0.0.1 <port>
```

## Expected Output

TODO — both operations invoked through the facade, plus the version
mismatch error demonstrated deliberately.

## What to Notice

- TODO: a facade is a naming/versioning layer, not a second execution
  engine — the underlying `Operation`s and their rollback behavior are
  unchanged from Lesson 9.
- TODO: identity is `FacadeId + version`, never the position in a list.

## Related Reference

- [FACADES.md](../../FACADES.md)
- [docs/reference/domain-operations.md](../../reference/domain-operations.md)
- [DEVELOPER_GUIDE.md, Chapter 11](../../DEVELOPER_GUIDE.md#11-facades-versioned-remote-surfaces)
