# Building an Employee Directory with Ring0

A project-based course: you build one application, an employee directory,
from an empty file to a networked, multi-client system with remote
operations, versioned facades, and an org-chart graph — one capability at a
time. Each lesson assumes you completed the previous one and starts from
that lesson's ending code.

This assumes you're comfortable with modern C++ (structs, templates,
lambdas, RAII, smart pointers, `std::filesystem`) and have already skimmed
[docs/DEVELOPER_GUIDE.md](../../DEVELOPER_GUIDE.md) or at least Chapter 0-1
of it (what Ring0 is, how to build it).

> **Status:** 🚧 skeleton course. Lessons below have their structure and
> learning goals written; step-by-step content and runnable code land
> lesson by lesson. Each lesson names its target reference doc(s) in
> `docs/reference/` — those are already written in full and safe to read
> ahead of the lesson prose.

## Lessons

1. [Binding Your First Type](01-binding-your-first-type.md) — define
   `Employee`, bind it, create the database.
2. [Persist and Reopen](02-persist-and-reopen.md) — real records that
   survive a restart.
3. [Transactions](03-transactions.md) — the commit/rollback contract, done
   properly.
4. [Handles and Updates](04-handles-and-updates.md) — give an employee a
   raise.
5. [Relationships: Departments and Projects](05-relationships.md) —
   `Ref<Department>`, an owned record, and a collection of projects.
6. [Consistent Reports with Snapshots](06-snapshots.md) — a payroll report
   that doesn't see half-finished edits.
7. [Searching with Queries and Indexes](07-queries-and-indexes.md) — find
   employees by department and salary, fast.
8. [Serving the Directory Over the Network](08-networking.md) — split into
   a server and a separate client.
9. [Remote Operations: Transferring an Employee](09-remote-operations.md) —
   business logic that runs on the server.
10. [Facades: A Stable HR API](10-facades.md) — a versioned surface the
    client calls without knowing server internals.
11. [The Org Chart: Graphs](11-graphs.md) — model and traverse
    manager/report relationships.
12. *(Optional, advanced)* [Asynchronous WAL I/O](12-async-io.md) — measure
    it before you reach for it.
13. *(Optional, advanced)* [A Read Replica for Reporting](13-read-replica.md)
    — offload reports to a follower.

## Build the Exercises

Each lesson names an executable target under `examples/employee_directory/`
once its code exists. Until then:

```powershell
cmake --preset debug
cmake --build --preset debug
```

## Conventions Used in Every Lesson

- **What You'll Add** — one paragraph, framed in terms of the running app,
  not the underlying mechanism.
- **New Concepts** — links into `docs/reference/` for the precise
  documentation; the lesson itself stays narrative and example-driven.
- **Starting Point** — what you're building on top of (the previous
  lesson's ending state).
- **Steps** — the actual walkthrough.
- **Full Listing** — the complete file(s) at the end of the lesson, so you
  can diff against your own attempt.
- **Build and Run** — the exact command and expected output.
- **What to Notice** — the one or two things worth remembering afterward.
