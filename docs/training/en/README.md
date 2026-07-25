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

> **Status:** ✅ complete — all 13 lessons have real, compiling code, one
> self-contained folder per lesson, verified against an actual build and
> run. Each lesson names its target reference doc(s) in `docs/reference/`
> — those are written in full and safe to read ahead of the lesson prose.

## Lessons

Each lesson lives in its own folder: the lesson doc, its `.cpp` source,
and a `README.md` with the exact build/run commands.

1. [Binding Your First Type](01-binding-your-first-type/01-binding-your-first-type.md) — define
   `Employee`, bind it, create the database.
2. [Persist and Reopen](02-persist-and-reopen/02-persist-and-reopen.md) — real records that
   survive a restart.
3. [Transactions](03-transactions/03-transactions.md) — the commit/rollback contract, done
   properly.
4. [Handles and Updates](04-handles-and-updates/04-handles-and-updates.md) — give an employee a
   raise.
5. [Relationships: Departments and Projects](05-relationships/05-relationships.md) —
   `Ref<Department>`, an owned record, and a collection of projects.
6. [Consistent Reports with Snapshots](06-snapshots/06-snapshots.md) — a payroll report
   that doesn't see half-finished edits.
7. [Searching with Queries and Indexes](07-queries-and-indexes/07-queries-and-indexes.md) — find
   employees by department and salary, fast.
8. [Serving the Directory Over the Network](08-networking/08-networking.md) — split into
   a server and a separate client.
9. [Remote Operations: Transferring an Employee](09-remote-operations/09-remote-operations.md) —
   business logic that runs on the server.
10. [Facades: A Stable HR API](10-facades/10-facades.md) — a versioned surface the
    client calls without knowing server internals.
11. [The Org Chart: Graphs](11-graphs/11-graphs.md) — model and traverse
    manager/report relationships.
12. *(Optional, advanced)* [Asynchronous WAL I/O](12-async-io/12-async-io.md) — measure
    it before you reach for it.
13. *(Optional, advanced)* [A Read Replica for Reporting](13-read-replica/13-read-replica.md)
    — offload reports to a follower.

## How the Code Is Organized

Each lesson is a folder — `01-binding-your-first-type/`,
`02-persist-and-reopen/`, ... `13-read-replica/` — holding three things:
the lesson doc (`<slug>.md`), one **self-contained** source file
(`lesson_01_binding.cpp`, `lesson_02_persist_reopen.cpp`, ...
`lesson_13_read_replica.cpp`, each containing only *that* lesson's own new
code), and a `README.md` with that lesson's build command.

The lessons chain to each other through one real, persistent database
file — `docs/training/en/employee-directory.modb` (git-ignored, like
every other `*.modb` in this repo) — instead of through shared in-process
state:

- **Lesson 1** deletes any old copy and creates this file fresh. Every
  other lesson only ever *opens* it.
- **Every lesson from 2 onward is a separate program run** that opens the
  same file, does its own work, and exits. Nothing is held in memory
  between lessons — the file on disk *is* the hand-off.
- Since each lesson is a separate binary, there's no shared struct to
  carry object ids forward (like an in-memory "here's Ana's id" value).
  Instead, Lesson 2 creates an **index on `Employee.name`**
  (`create_index<Employee>(FieldId{1})`), and every later lesson looks
  employees up by name (`indexed_object_ids<Employee>(...)`) instead of
  remembering ids across runs. Lesson 5 does the same for
  `Department.name`, needed starting at Lesson 9.
- Because of this, **lessons must be run in order, once each**, starting
  from Lesson 1 — running Lesson 5 before Lesson 2 fails with a clear
  "have you run the earlier lessons first?" message, since the records
  and indexes it expects don't exist yet.
- Object ids, salaries, and other printed values are still deterministic
  given a full, in-order run — the same sequence of creates/removes
  always produces the same ids — except where a lesson explicitly
  measures wall-clock time (Lesson 12).
- To restart the whole course, just re-run Lesson 1 — it wipes the file
  and starts over.

Lessons 8, 9, and 10 (networking, remote operations, facades) keep the
server and the client in **one binary**: a background thread runs the
server's accept loop while the main thread drives the client connection.
A real deployment would split these into separate processes, but nothing
in `Server`/`ServerConnection` requires it.

## Build the Exercises

Every lesson has its own CMake target, `employee_directory_lesson_NN`,
and its own folder's `README.md` repeats the exact command for that
lesson:

```powershell
cmake --preset debug
cmake --build --preset debug --target employee_directory_lesson_01
.\build\debug\employee_directory_lesson_01.exe
```

Swap `01` for any lesson number 01-13. *Building* a later lesson's target
does not require building earlier ones first — each `.cpp` file compiles
on its own. *Running* them, though, is in order: Lesson `NN` expects
Lessons 1 through `NN-1` to have already run against the same persistent
file. To go through the whole course from scratch:

```powershell
cmake --preset debug
cmake --build --preset debug
foreach ($n in 1..13) {
    & ".\build\debug\employee_directory_lesson_$('{0:D2}' -f $n).exe"
}
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
