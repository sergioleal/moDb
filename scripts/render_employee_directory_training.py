#!/usr/bin/env python3
"""Render the employee-directory training lessons (docs/training/en) as
static HTML, in the same visual model used by the older phase-based
course now archived under docs-process/training/.

Unlike that older course, this one's lessons live one-per-folder
(docs/training/en/<NN-slug>/<NN-slug>.md, plus that folder's own
lesson_NN_*.cpp and README.md build guide) instead of one flat
directory, and each lesson's source is self-contained -- no cumulative
copy-paste -- chained through one persistent database file. Each
lesson's rendered HTML (and its narration script) is written into that
SAME lesson folder, right next to its .md/.cpp/README.md, rather than
into a separate flat html/ tree -- one more thing keeping every lesson
folder self-contained. Only the course-level index.html and the shared
CSS/logo assets live at the course root, docs/training/en/, alongside
README.md.

This script does NOT produce narration audio (.mp3) files -- only the
textual narration script per lesson, exactly like the older course's own
render_training.py, which only ever wrote the .txt narration and left the
audio/ directory for a human to fill in later.
"""

from __future__ import annotations

import html
import posixpath
import re
import shutil
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "docs" / "training" / "en"
ASSET_SOURCE = ROOT / "docs-process" / "training" / "assets"
ASSET_OUTPUT = SOURCE_DIR / "assets"

# (path relative to SOURCE_DIR, fallback title)
LESSONS = [
    ("README.md", "Index"),
    ("01-binding-your-first-type/01-binding-your-first-type.md", "Lesson 1"),
    ("02-persist-and-reopen/02-persist-and-reopen.md", "Lesson 2"),
    ("03-transactions/03-transactions.md", "Lesson 3"),
    ("04-handles-and-updates/04-handles-and-updates.md", "Lesson 4"),
    ("05-relationships/05-relationships.md", "Lesson 5"),
    ("06-snapshots/06-snapshots.md", "Lesson 6"),
    ("07-queries-and-indexes/07-queries-and-indexes.md", "Lesson 7"),
    ("08-networking/08-networking.md", "Lesson 8"),
    ("09-remote-operations/09-remote-operations.md", "Lesson 9"),
    ("10-facades/10-facades.md", "Lesson 10"),
    ("11-graphs/11-graphs.md", "Lesson 11"),
    ("12-async-io/12-async-io.md", "Lesson 12"),
    ("13-read-replica/13-read-replica.md", "Lesson 13"),
    ("14-cli-and-diagnostics/14-cli-and-diagnostics.md", "Lesson 14"),
    ("15-storage-internals/15-storage-internals.md", "Lesson 15"),
    ("16-blobs-sets-and-maps/16-blobs-sets-and-maps.md", "Lesson 16"),
    ("17-catalog-and-baselines/17-catalog-and-baselines.md", "Lesson 17"),
    ("18-protocol-and-compatibility/18-protocol-and-compatibility.md", "Lesson 18"),
    ("19-replication-catchup-walonly/19-replication-catchup-walonly.md", "Lesson 19"),
    ("20-performance-and-hardening/20-performance-and-hardening.md", "Lesson 20"),
]

# Basenames of markdown files that are part of THIS rendered set -- used to
# decide whether a `.md` link should become a sibling `.html` link, or stay
# a markdown link (external references keep pointing at their real .md
# file, since output now lives at the same depth as the source).
RENDERED_BASENAMES = {Path(name).name for name, _ in LESSONS}

LESSON_NOTES = {
    "README.md": {
        "context": (
            "This course is the project-based companion to docs/DEVELOPER_GUIDE.md. Instead of "
            "a fast narrative tour, you build one real application -- an employee directory -- "
            "one capability at a time, and every lesson's code is real, compiling code, not "
            "pseudocode."
        ),
        "goal": (
            "Set expectations for the whole arc -- binding, persistence, transactions, schema "
            "evolution, relationships, snapshots, queries, networking, remote operations, "
            "facades, graphs, async I/O, and replication -- and explain how the 13 lessons "
            "chain together."
        ),
        "outcomes": [
            "Understand why each lesson is a separate, self-contained program that opens the same persistent file the previous lesson left behind.",
            "See how object identity is recovered across separate runs through name-based lookups instead of shared in-process state.",
            "Know where each lesson's source and build instructions live.",
        ],
        "narration": """Think of this course as watching one small, real application grow, one commit-sized capability at a time, instead of reading thirteen disconnected demos.

Every lesson lives in its own folder: the lesson document, one self-contained C++ source file, and a short README explaining exactly how to build and run it. "Self-contained" is a specific claim here -- each source file contains only that lesson's own new code, nothing copy-pasted forward from earlier lessons.

What makes the lessons a course rather than thirteen unrelated examples is a single, real, persistent database file. Lesson 1 creates it. Every lesson after that is a separate program run that opens the very same file and continues exactly where the previous run left off. Ana, Bruno, and Carla, once created in Lesson 2, are still there in Lesson 13.

Because each lesson is a genuinely separate process with no shared memory, you will also see a recurring pattern: instead of remembering an employee's numeric id across runs, later lessons look employees up by name through an index. That is not a toy simplification -- it is closer to how a real client actually finds a record after restarting.

Run the lessons in order, once each, starting from Lesson 1. If you want to start over, just re-run Lesson 1 -- it deletes any old copy of the file and creates it fresh.""",
    },
    "01-binding-your-first-type.md": {
        "context": (
            "Before an employee directory can store anything, Ring0 needs to know what an "
            "Employee looks like. That mapping between a plain C++ struct and the catalog is "
            "called a binding."
        ),
        "goal": (
            "Define `Employee`, bind it with `BindingBuilder<Employee>`, create the database file "
            "that every later lesson will keep reopening, and confirm the type is registered."
        ),
        "outcomes": [
            "Read a `BindingBuilder` definition and identify the persistent field ids.",
            "Explain why field ids, not field names, are the durable contract.",
            "Understand that binding a type is a separate step from writing any record of it.",
        ],
        "narration": """This is the first lesson, so it starts from nothing: no file, no schema, no records.

The `Employee` struct is ordinary C++ -- a `name` and a `salary`, nothing framework-specific. The interesting part is `employee_binding()`, which maps field id 1 to `name` and field id 2 to `salary` through a `BindingBuilder<Employee>`. Those small integers are what actually get persisted alongside every record; the field names exist for the reader's benefit, not the database's.

The lesson creates the database file at a fixed, permanent location -- `docs/training/en/employee-directory.modb` -- deletes any old copy first, binds `Employee`, and prints the `TypeDefinitionId` the catalog assigned. Nothing is written as a durable record yet; that is Lesson 2's job.

What this lesson establishes, more than any specific API, is the starting point for the whole course: one file, on disk, that every subsequent lesson will reopen. Detaching from the registry at the end does not delete that file -- it stays exactly where it is, waiting for Lesson 2.""",
    },
    "02-persist-and-reopen.md": {
        "context": (
            "A binding only describes a shape. The next question is whether a real record survives "
            "closing and reopening the file Lesson 1 created."
        ),
        "goal": (
            "Write three employees in one transaction, then reopen the file in a second block and "
            "read one of them back -- found by name, not by a remembered id."
        ),
        "outcomes": [
            "Understand `Database::open` versus `Database::create`, and why this lesson only ever opens.",
            "See why reopening is a stronger persistence proof than reading data still in memory.",
            "Explain why an index on `Employee.name` becomes necessary the moment lessons stop sharing a process.",
        ],
        "narration": """This lesson is two scoped blocks inside one `main()`, and the boundary between them matters more than it looks.

The first block opens the file Lesson 1 created -- never creates one of its own -- begins a transaction, and commits three `Employee` records: Ana, Bruno, and Carla. Right after that commit, it also creates an index on `Employee.name`. That index is not this lesson's teaching topic; it is infrastructure every later lesson depends on, because every later lesson is a genuinely separate program run with no access to the `ObjectId`s this run just produced.

The second block reopens the file again, as if it were a fresh process starting up, and looks Ana up by name through that index rather than by remembering her id. It then confirms the id it found matches the one this same run captured earlier, proving the lookup is not just plausible but correct.

The object ids you will see (18, 19, 20) are deterministic for this exact sequence of operations, but they are not "1, 2, 3" -- the catalog itself consumes a few ids first, for its own type definitions and baselines, before your first real record.""",
    },
    "03-transactions.md": {
        "context": (
            "Employees now exist and can be found by name. The next question a real application "
            "has to answer is what \"give someone a raise, correctly\" actually requires."
        ),
        "goal": (
            "Commit one raise properly, deliberately let a second raise roll back by never "
            "calling `commit()`, touch two employees atomically in one transaction, and show a "
            "second `begin()` failing while the first is still open."
        ),
        "outcomes": [
            "Explain rollback-on-scope-exit as a safety net, not a bug to guard against.",
            "Describe why touching Ana and Bruno's salaries in one transaction is safer than two separate writes.",
            "State, in one sentence, what single-writer means for this application.",
        ],
        "narration": """This lesson opens by looking Ana, Bruno, and Carla up by name -- the same pattern Lesson 2 introduced, now used for real work instead of just proving it works.

Bruno's raise is unremarkable and that is the point: begin a transaction, materialize, mutate the salary field, update, commit. Carla's raise looks almost identical, except the transaction is deliberately allowed to go out of scope without ever calling `commit()`. Nothing crashes. Nothing throws. The change simply never becomes durable, and reading Carla back afterward proves it.

The third scenario touches two employees, Ana and Bruno, inside one transaction -- averaging their salaries -- to make atomicity concrete rather than abstract: either both salaries change together, or neither does.

The last scenario is the sharpest one: opening a second transaction while the first is still active fails immediately, with a message that says exactly that. This application, like the database underneath it, has one writer at a time. Concurrent readers are unaffected; concurrent writers are not.""",
    },
    "04-handles-and-updates.md": {
        "context": (
            "Every employee so far has the same two-field shape from Lesson 1. Real applications "
            "eventually need to add a field to a type that already has real, persisted records."
        ),
        "goal": (
            "Introduce `EmployeeV2` with a `country` field defaulted to `\"BR\"`, observe real "
            "schema evolution against Ana's existing 2-field record, then raise her salary through "
            "a typed `Handle` instead of manual materialize-mutate-update."
        ),
        "outcomes": [
            "Distinguish a divergent-shape re-`bind()` from a purely additive one.",
            "Explain why Ana's `country` comes from a declared default, not from disk.",
            "Use `Handle<T>::set<&T::member>(tx, value)` and say what it does under the hood.",
        ],
        "narration": """By the time this lesson runs, Ana, Bruno, and Carla already exist as plain 2-field records, written back in Lessons 2 and 3. That is exactly what makes this lesson's schema evolution real rather than staged: `EmployeeV2` adds a third field, `country`, defaulted to `"BR"`, and binding it under the same catalog name `"Employee"` is a genuinely divergent shape from what is already on disk.

Reading Ana back through the new binding prints her with `country=BR` -- not because that value was ever written, but because the binding declares it as the default for records that predate the field. Nothing on disk was rewritten just by reading it this way.

The raise that follows uses `Handle<EmployeeV2>::set<&EmployeeV2::salary>(tx, new_salary)` instead of the manual materialize-mutate-`update()` sequence from Lesson 3. The member-pointer expression lets the compiler participate in selecting the field; the transaction underneath still does exactly the same work as before.

After this raise commits, Ana's record is physically stored in the full 3-field shape -- the first write she has had since the schema changed is also the write that migrates her.""",
    },
    "05-relationships.md": {
        "context": (
            "An employee directory with no departments, no emergency contacts, and no projects "
            "is not really a directory yet. This lesson introduces the relationships that make it one."
        ),
        "goal": (
            "Add `Department` (via `Ref`), an owned `EmergencyContact` (via `OwnedRef`, "
            "cascade-deleted with its owner), and a `PersistentVector<Ref<Project>>` -- then "
            "watch a dangling reference happen and resolve to a clear error."
        ),
        "outcomes": [
            "Distinguish `Ref<T>` (association) from `OwnedRef<T>` (ownership) by their delete behavior.",
            "Explain why a `PersistentVector`'s `BlobId` must be re-stored on its owner after every mutation.",
            "Describe what happens when you remove an object something else still points to.",
        ],
        "narration": """This is the lesson where the directory stops being three isolated salary records and starts looking like an organization.

`Department` is the simplest new type here: just a name, referenced from `Employee` through `Ref<Department>`. Ana and Bruno get assigned to Engineering and Sales respectively, and -- quietly, as infrastructure rather than as this lesson's topic -- `Department.name` gets its own index, for the same reason `Employee.name` got one back in Lesson 2.

Next comes a temporary employee, Diego, with an `OwnedRef<EmergencyContact>` pointing at his contact, Elena. Removing Diego removes Elena too, automatically -- that is what `OwnedRef` means. Trying to resolve her afterward fails with `record_not_found`, and that failure is the proof, not a side note.

Carla is assigned two projects through a `PersistentVector<Ref<Project>>` -- a collection stored as a separate blob, referenced from `Employee` by a `BlobId` field that has to be re-stored on Carla every time the vector's identity changes underneath it.

Finally, Sales is removed while Bruno's `department` field still points at it. Nothing prevents the removal and nothing auto-repairs Bruno's reference. Resolving Sales directly afterward fails cleanly -- and Bruno's field keeps pointing at a department that no longer exists, a loose end this course does not tie up until Lesson 9.""",
    },
    "06-snapshots.md": {
        "context": (
            "Departments, an owned contact, and project assignments are now real. The next "
            "question is what a payroll report should see while other changes keep happening."
        ),
        "goal": (
            "Take a snapshot before running a payroll report, show a concurrent raise doesn't "
            "change the report's total, force a `snapshot_conflict` on purpose, retry once the "
            "snapshot closes, and call `collect_garbage()`."
        ),
        "outcomes": [
            "Explain a `Snapshot` as an epoch number, not a copy of the database.",
            "Describe what `snapshot_conflict` means and why retrying is the right response.",
            "Connect open snapshots to what `collect_garbage()` is and is not allowed to reclaim.",
        ],
        "narration": """This lesson opens a `Snapshot`, sums every employee's salary through it, then -- deliberately, while that same snapshot is still open -- commits a raise for Carla in a fresh transaction.

Reading the payroll total again through the *same* snapshot returns the exact same number as before. That is not a bug or a timing accident; it is the entire point of a snapshot. It answers questions as of the moment it was opened, no matter what commits afterward.

A second raise attempt for Carla, still with that snapshot open, fails outright with `snapshot_conflict`. The message names the object and explains why: an older open snapshot still needs to see its previous version. The fix is not to fight the conflict -- it is to let the blocking snapshot close and retry, which is exactly what this lesson does next, successfully.

A fresh snapshot afterward reports the new, higher total. Finally, `collect_garbage()` reclaims the old, now-unreferenced versions that the earlier snapshot had been protecting -- object versions this lesson's own history had accumulated all the way back to Lesson 3.""",
    },
    "07-queries-and-indexes.md": {
        "context": (
            "Scanning every employee by hand does not scale, and it is not how a real directory "
            "would answer \"who earns at least this much?\" This lesson replaces hand-written "
            "loops with `Database::query<T>()`."
        ),
        "goal": (
            "Run the same salary search first as a table scan, then again after "
            "`create_index<Employee>(salary_field)`, and compare `.plan()` output both times "
            "before running a `top_k` query for the highest earners."
        ),
        "outcomes": [
            "Read a `QueryPlan`'s `access_name()` and `index_available` fields.",
            "Explain why the exact same query can report `table_scan` and, later, `index_scan`.",
            "Distinguish `top_k` from sorting the whole result set."
        ],
        "narration": """This lesson runs the identical query twice, and the interesting part is precisely that it is identical both times.

The first run -- `.between(salary_field, 15000, max_int64)` -- has no index to work with, so `.plan()` reports `access=table_scan`. Nothing about the query itself asked for a scan; there was simply nothing better available yet.

`create_index<Employee>(salary_field)` changes exactly one thing: whether an index exists. Running the *same* query again reports `access=index_scan`. The planner did not need a hint, a rewritten query, or a different API call -- it is rule-based, and an index appearing is, on its own, enough to change the chosen access path.

A `top_k(2, comparator)` query closes the lesson, returning the two highest earners with their salaries. `top_k` gets its own access path rather than sorting the entire table and taking the first two -- which is why it is worth learning as a distinct tool, not a shorthand for `order_by` plus `limit`.""",
    },
    "08-networking.md": {
        "context": (
            "Everything so far has run inside one process talking to its own file. Real "
            "directories get queried by other programs, over a network."
        ),
        "goal": (
            "Start a `Server` on a background thread, connect as a client with "
            "`ServerConnection`, run a capability handshake, and repeat the salary search from "
            "Lesson 7 remotely through `QueryDescription`."
        ),
        "outcomes": [
            "Describe the single-binary server/client convention this and the next two lessons use.",
            "Explain why `QueryDescription` only supports one `EqualityFilter`, not `.between()`.",
            "Read a handshake's negotiated protocol version and stream limit.",
        ],
        "narration": """This lesson keeps the server and the client in one binary: a background thread runs `serve_one()`, accepting exactly one connection, while the main thread plays the client. A real deployment would split these into separate processes, but nothing about `Server` or `ServerConnection` requires that split, and keeping them together here keeps the lesson's code easy to read top to bottom.

`Server::listen(path, "127.0.0.1", 0)` opens the same persistent file every earlier lesson has been building up -- port zero means "pick any free port." The client connects, and the first thing it does is not query anything at all -- it reads the negotiated protocol version and `max_concurrent_streams` from the handshake, the network-level version of the compatibility habit that runs through this whole product.

Only after that does the client collect every employee remotely, then repeat Lesson 7's exact-salary search -- but now through `QueryDescription`'s `EqualityFilter`, not the local `Query<T>` builder's `.between()`. That is a deliberate narrowing: the wire protocol supports a type, an optional limit, and one equality filter, nothing richer. If you need `.between()`, `top_k`, or composed conditions over the network, that gap is exactly what Lessons 9 and 10 exist to close.""",
    },
    "09-remote-operations.md": {
        "context": (
            "Bruno's department has pointed at a removed record since Lesson 5. Querying can find "
            "employees remotely, but it cannot fix that -- fixing it is business logic, and "
            "business logic belongs on the server."
        ),
        "goal": (
            "Define a `TransferDepartment` operation that validates its target before mutating "
            "anything, register it through an `OperationRegistry` and `ModuleManifest`, and call "
            "it from a remote client to finally move Bruno to Engineering."
        ),
        "outcomes": [
            "Explain why `execute()` checks the target department before touching the employee.",
            "Describe what a client actually links against when it calls a remote operation.",
            "State what a rejected `execute()` call does to any change the operation might have made.",
        ],
        "narration": """This lesson starts by looking up two things by name: Bruno, and the department this lesson is finally going to move him to, Engineering -- found through the `Department.name` index Lesson 5 quietly set up for exactly this moment.

`TransferDepartment` is a small `Operation` class: an id string, argument encoding for an employee id and a department id, and an `execute()` that checks the target department exists *before* it materializes or mutates the employee. That ordering is the whole design: validate first, mutate second, so a rejected call never leaves a half-applied change behind.

Registering it is a short but specific sequence -- build a `ModuleManifest`, compute its hash, admit that hash on a `ModuleLoader`, load it into an `OperationRegistry`, and attach that registry to the server. The client, meanwhile, never links against `TransferDepartment`'s implementation at all. It only knows the operation's id string and how to encode its arguments.

The successful call moves Bruno to Engineering, closing the dangling reference Lesson 5 left open. The second call, aimed at a department id that does not exist, fails with a clear message -- and leaves Bruno's `department` field exactly as the first call left it.""",
    },
    "10-facades.md": {
        "context": (
            "`TransferDepartment` works, but calling it means knowing its exact id string and "
            "argument encoding. Real client SDKs want a stable, typed surface instead."
        ),
        "goal": (
            "Add a second operation, `GiveRaise`, and group both operations behind one versioned "
            "`HRFacade` that a client opens once and invokes typed methods through -- then show a "
            "mismatched facade version being rejected outright."
        ),
        "outcomes": [
            "Explain what a `FacadeDescriptor` adds on top of an `OperationRegistry` entry.",
            "Describe why identity is `FacadeId + version`, not a position in a list.",
            "State what changes, and what does not, compared to calling `TransferDepartment` directly in Lesson 9.",
        ],
        "narration": """`GiveRaise` is built exactly like `TransferDepartment` was -- an id, argument encoding, an `execute()` that materializes, mutates, and updates. Nothing about individual operations changes in this lesson.

What is new is `HRFacade`: a tag type naming a facade id and a version, backed by a `FacadeDescriptor` that lists both `TransferDepartment` and `GiveRaise` as its methods. The server registers this facade in a `FacadeCatalog` alongside the operation registry it already had. The client opens the facade once, gets back a typed handle, and calls `invoke<TransferDepartment>(...)` and `invoke<GiveRaise>(...)` through it -- moving Bruno back to Engineering and giving Ana a raise, both through the same handle.

The version-mismatch demonstration is the sharpest part of the lesson: a second tag, `HRFacadeV2`, asks for a version of "hr" the server never registered. It is rejected -- not because "hr" is unknown, but because no version 2 of it exists. Identity here is the pair, facade id plus version, never a position in a list or an assumption about what the newest version must be.

A facade is a naming and versioning layer, not a second execution engine. Everything `invoke<Method>` does still travels through the exact same operation dispatch Lesson 9 introduced.""",
    },
    "11-graphs.md": {
        "context": (
            "Employees already point at departments and projects. An organization also has a "
            "reporting chain, and \"who reports to whom\" is a graph, not a flat list."
        ),
        "goal": (
            "Add a `manager: Ref<Employee>` field, walk the org chart downward from a manager "
            "with `graph::bfs` and `GraphView::incoming`, then create a dangling manager "
            "reference and compare the three `DanglingPolicy` behaviors against it."
        ),
        "outcomes": [
            "Explain why `incoming()` needs an index on the `Ref` field it walks.",
            "Describe the difference between walking \"down\" (reports) and \"up\" (manager) through the same field.",
            "State what `DanglingPolicy::fail`, `skip`, and `yield_error` each do to a traversal.",
        ],
        "narration": """Adding `manager: Ref<Employee>` is this run's fourth schema evolution, after Lesson 4's `country` and Lesson 5's three relationship fields -- and, like every field before it, its default is a zero-valued `Ref`, so existing records keep projecting cleanly.

Walking the org chart downward from a manager uses `GraphView::incoming`, wrapped in an `AdjacencyFn` that `graph::bfs` drives: given a manager, find every employee whose `manager` field points back at them. That direction needs an index on the field, which this lesson creates right alongside the schema change, and it can never report a "dangling" result -- a source that no longer resolves is just silently skipped.

To see dangling handled deliberately, the lesson walks the *other* direction instead: a temporary employee, Gustavo, reports to another temporary employee, Felipe, using `graph::edge()` directly over the single `manager` field. Felipe is then removed, and walking up from Gustavo hits a target that no longer exists.

Three traversals of that same broken edge, back to back, show what each `DanglingPolicy` actually does: `fail` stops the walk with an error, `skip` ends quietly with nothing reported, and `yield_error` surfaces the error but would have kept exploring any other, unrelated branch -- a difference this particular one-edge case cannot show, since Gustavo only has the one dangling edge to walk.""",
    },
    "12-async-io.md": {
        "context": (
            "Every commit so far has used Ring0's default, synchronous WAL writer. This optional, "
            "advanced lesson asks whether an asynchronous one is actually faster for this "
            "workload."
        ),
        "goal": (
            "Reopen the same persistent file twice -- once with default `DatabaseOptions`, once "
            "with `wal_io = WalIoMode::async` -- and time 200 committed raises under each, "
            "reporting honestly whichever mode wins."
        ),
        "outcomes": [
            "Explain why `wal_io` is a per-open runtime choice, not a persisted format decision.",
            "State this lesson's actual measured result, and what it does and does not prove.",
            "Describe why \"measure your own workload\" is the lesson's real takeaway, not a specific mode.",
        ],
        "narration": """This lesson is unusual in the course: there is nothing new for the employee directory to *do*. The measurement itself is the point.

`wal_io` lives in `DatabaseOptions`, and it is a per-open runtime choice -- not something baked into the file's format the way a schema evolution is. That is what lets this lesson reopen the exact same file twice, once under the default synchronous WAL writer and once under `WalIoMode::async`, and time 200 committed raises against Carla under each.

The two timings are printed side by side along with commits per second, and the lesson reports honestly whichever mode came out ahead on that particular run -- on the measurement this course captured, the default synchronous mode was marginally faster, consistent with this product's own operational notes describing "no consistent win yet" for the async path.

Carla's salary is reset to a clean value at the end, so the timing loop's last iteration does not leave a strange number behind for Lesson 13's payroll report to pick up. The lesson's real lesson is not "async is slower" -- it is that an option existing is not a reason to reach for it before measuring.""",
    },
    "13-read-replica.md": {
        "context": (
            "The whole course has run against one file that both reads and writes. Reporting "
            "workloads often want to run somewhere that cannot compete with live writes -- a "
            "read-only follower."
        ),
        "goal": (
            "Bootstrap a follower from the primary using the same library calls behind "
            "`modb replicate bootstrap`, confirm its identity matches the primary, run a "
            "Lesson-6-style payroll report against it, and confirm it refuses writes."
        ),
        "outcomes": [
            "Describe what `create_bootstrap_snapshot`'s writer barrier is, and is not.",
            "Explain why the follower here is this lesson's own throwaway artifact, unlike every earlier lesson's persistent file.",
            "State what a `wal_only` primary cannot do, and what would be needed instead.",
        ],
        "narration": """This is the last lesson, and its follower is deliberately not part of the chain the way the primary file has been for the previous twelve. It is created fresh, used, and removed again before the run ends -- there is no Lesson 14 to hand it forward to.

`create_bootstrap_snapshot(primary, temp_dir)` briefly opens and rolls back a transaction on the primary purely as a writer barrier, then copies its data file. `install_bootstrap_snapshot` moves that copy into place as a separate file, `employee-directory-replica.modb`, next to the main directory file -- never the same path, never the same handle.

Opening the follower, confirming its `database_uuid()` matches the primary's, and calling `set_read_only_replica(true)` sets up the rest of the lesson: a payroll report -- the exact same `scan<Employee>(snapshot, visitor)` pattern from Lesson 6 -- run against the follower's own snapshot instead of the primary's, and a `begin()` call on the follower that fails immediately with `replica_read_only`.

What this lesson does not implement is the streaming half of replication -- keeping a follower continuously caught up after its initial bootstrap -- or a `wal_only` primary, which cannot donate a file-copy bootstrap at all and needs `seed-wal` instead. Both extend naturally from what is here, but they are out of this lesson's scope.""",
    },
}

CONCEPT_DEEP_DIVE = {
    "README.md": (
        "Most tutorials show independent snippets: a query example here, a networking example "
        "there, each starting from a clean slate. That is easy to write but does not resemble how "
        "software actually accumulates. This course instead commits to one persistent artifact -- "
        "a single database file -- and every lesson's code must cope with whatever state the file "
        "already holds, the same constraint a real production system lives under permanently. "
        "Object identity recovered by name lookup, not by remembered ids, is a direct consequence "
        "of taking that constraint seriously: a real client restarting cannot remember an id "
        "either."
    ),
    "01-binding-your-first-type.md": (
        "A binding is Ring0's answer to the gap between C++ memory layout and a durable schema. "
        "The struct compiles into one program; the bytes it produces may be read years later by a "
        "different build. The binding is what tells Ring0 which fields belong to the persistent "
        "contract, and stable field ids -- not field names, not member order -- are the coordinate "
        "system that makes old bytes interpretable with intent long after the code that wrote them "
        "has changed."
    ),
    "02-persist-and-reopen.md": (
        "Reading data back immediately after writing it only proves that memory allocation and an "
        "API call succeeded. Reopening the file proves a longer chain: the record was encoded, "
        "written, committed, made findable by identity, found again, and decoded through a "
        "binding registered fresh in a new process. The name index that appears here for the first "
        "time exists precisely because that chain, across genuinely separate program runs, has no "
        "shared memory to fall back on -- identity has to be recoverable from the data itself."
    ),
    "03-transactions.md": (
        "A transaction is a promise about grouping: either an intended change becomes visible as a "
        "coherent unit, or none of it does. Forgetting `commit()` is not a defect to code "
        "defensively against -- it is the mechanism working exactly as designed, and this lesson "
        "treats it as a demonstration rather than an error case. Single-writer, meanwhile, is a "
        "deliberate simplicity: concurrent readers can proceed freely, but only one writer is ever "
        "mid-transaction, which is what makes the two-employee average in this lesson provably "
        "atomic rather than merely likely to be."
    ),
    "04-handles-and-updates.md": (
        "Schema evolution only means something when it happens against records that already "
        "exist -- which is why this lesson does not fabricate old data, it relies on Ana's real "
        "2-field record from Lessons 2 and 3. `Handle<T>` is a typed reference to durable identity, "
        "and its `set<&T::member>` selects a field through the type system instead of a string, but "
        "it still cannot bypass the transaction underneath. The compiler helps you name the field "
        "correctly; the database still decides whether the write becomes durable."
    ),
    "05-relationships.md": (
        "An object database earns its keep when the domain has relationships, not just isolated "
        "records. `Ref` and `OwnedRef` are wire-identical -- both are stored as an `ObjectId` -- but "
        "they encode different intent: association versus ownership, which is why only one of them "
        "cascades on delete. The dangling reference left behind when Sales is removed is not a bug "
        "this lesson works around; it is shown deliberately, because a real system has to decide "
        "what \"still points at something gone\" means, and silently preventing or auto-repairing it "
        "would hide that decision rather than make it."
    ),
    "06-snapshots.md": (
        "A snapshot separates time from mutation: without one, a reader's idea of \"the database\" "
        "can shift mid-read because of concurrent commits. With one, a reader asks for the state as "
        "of a specific logical moment and keeps getting consistent answers from it, no matter what "
        "commits afterward. `snapshot_conflict` is the price of that guarantee when a write and an "
        "open snapshot collide on the same object -- and it is a signal to retry once the snapshot "
        "closes, not a hard failure to avoid by holding snapshots open as briefly as possible in "
        "the first place."
    ),
    "07-queries-and-indexes.md": (
        "Ring0's planner is rule-based, not cost-based: the same query shape always produces the "
        "same plan, and only the presence or absence of an index changes the outcome. That "
        "determinism is what lets this lesson demonstrate the effect of `create_index` so cleanly "
        "-- nothing about the query changed between the two `.plan()` calls, only what the planner "
        "had available. `top_k` earning its own access path, rather than being sugar over a full "
        "sort, is the same idea applied to a different question: give the planner a shape it can "
        "recognize, and it can choose a better strategy for it."
    ),
    "08-networking.md": (
        "Crossing a process boundary changes what a client is allowed to assume. Inside one "
        "process, a lambda can inspect an object directly; across a connection, a request has to "
        "be described in a form the server can execute without importing the client's code. "
        "`QueryDescription` is that boundary object, and its narrowness -- one type, one optional "
        "limit, one optional equality filter -- is not an oversight. It is the minimum the wire "
        "protocol commits to, which is exactly why remote operations and facades exist for "
        "everything richer."
    ),
    "09-remote-operations.md": (
        "Moving a client-visible mutation onto the server is not just an implementation detail -- "
        "it is where an invariant gets enforced exactly once instead of reimplemented by every "
        "caller. If clients mutated `department` directly, each one would need to remember to check "
        "the target exists first; by making that check part of `TransferDepartment::execute()`, the "
        "rule lives with the data it protects. The client's job shrinks to expressing intent -- an "
        "id and an encoding -- while the server owns both the validation and the transaction "
        "boundary around it."
    ),
    "10-facades.md": (
        "Operations are the executable unit; a facade is the product-facing surface built on top "
        "of them. That distinction matters as a system grows, because operations can be added, "
        "reused, and composed by more than one facade, while a facade gives one particular class "
        "of consumer -- here, an HR client -- a stable, versioned vocabulary to depend on. Identity "
        "being `FacadeId + version`, rather than a list position, is what makes that stability "
        "meaningful: a client compiled against a version the server never published fails to open "
        "the facade at all, instead of silently receiving a surface it was not built for."
    ),
    "11-graphs.md": (
        "A reporting chain is a graph whether or not the storage layer calls it one -- `manager` is "
        "just another `Ref` field, and the graph module walks it without requiring a dedicated "
        "graph-shaped type. The asymmetry between the two directions is the real lesson: `incoming` "
        "(who reports to me) needs an index and can never itself observe a dangling result, because "
        "a stale or missing source is simply filtered out of the index scan, while the single "
        "`manager` field walked upward can point at something gone, which is precisely why the "
        "dangling-policy comparison has to happen in that direction and not the other."
    ),
    "12-async-io.md": (
        "The value of a controlled, honest measurement is that it can report a negative result "
        "without embarrassment. `wal_io` being a per-open, per-process choice rather than a file "
        "format decision is what makes the comparison possible at all in one lesson -- the same "
        "bytes on disk work under either mode. Reporting whichever mode wins on this run, instead "
        "of asserting a expected winner, is the entire point: opt-in performance features earn "
        "their place in a real system by being measured against a real workload, not by existing."
    ),
    "13-read-replica.md": (
        "A read-only replica is valuable precisely because it competes with nothing: no write "
        "traffic touches it, so a reporting workload running against it cannot be slowed down by, "
        "or slow down, the primary's live writes. The bootstrap's writer barrier is a brief, "
        "necessary compromise -- copying a consistent data file requires the primary to hold still "
        "for a moment -- but it is not a lasting lock, and it is the only point at which primary "
        "and follower ever interact in this lesson. Everything after that, the payroll report and "
        "the rejected write, happens entirely on the follower's own terms."
    ),
}


def output_relpath(markdown_relpath: str) -> str:
    """The rendered .html file's path, relative to SOURCE_DIR -- e.g.
    "index.html" for the course README, or
    "01-binding-your-first-type/01-binding-your-first-type.html" for a
    lesson. This lives in the SAME folder as the lesson's own .md/.cpp
    source, one more thing keeping that folder self-contained."""
    if markdown_relpath == "README.md":
        return "index.html"
    return str(PurePosixPath(markdown_relpath).with_suffix(".html"))


def narration_relpath(markdown_relpath: str) -> str:
    out = PurePosixPath(output_relpath(markdown_relpath))
    return str(out.with_name(out.stem + "-narration.txt"))


def audio_relpath(markdown_relpath: str) -> str:
    return str(PurePosixPath(output_relpath(markdown_relpath)).with_suffix(".mp3"))


def relative_href(from_relpath: str, to_relpath: str) -> str:
    """A relative href from the page at `from_relpath` to the page/asset
    at `to_relpath`, both given relative to SOURCE_DIR."""
    from_dir = str(PurePosixPath(from_relpath).parent)
    # posixpath.relpath, not os.path.relpath: hrefs must use "/" regardless
    # of which OS renders this course. os.path.relpath emits "\" on
    # Windows, and PurePosixPath doesn't treat "\" as a separator, so it
    # would pass a literal backslash straight into the href.
    return posixpath.relpath(to_relpath, start=from_dir)


def notes_key(markdown_relpath: str) -> str:
    # LESSON_NOTES/CONCEPT_DEEP_DIVE are keyed by basename (e.g.
    # "01-binding-your-first-type.md"), not the full "<slug>/<slug>.md"
    # relative path used elsewhere to locate the source file.
    return Path(markdown_relpath).name


def inline_markup(text: str, current_relpath: str) -> str:
    escaped = html.escape(text)
    escaped = re.sub(r"`([^`]+)`", r"<code>\1</code>", escaped)
    escaped = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", escaped)
    escaped = re.sub(r"\*([^*]+)\*", r"<em>\1</em>", escaped)
    escaped = re.sub(
        r"\[([^\]]+)\]\(([^)]+)\)",
        lambda match: replace_link(match, current_relpath),
        escaped,
    )
    return escaped


# Inline links -- i.e. everything replace_link() renders, all of it inside
# lesson prose -- open in a new tab so a reader following a reference
# (another lesson, DEVELOPER_GUIDE.md, a .cpp source) doesn't lose their
# place in the current lesson. Course navigation (Previous/Next/Index, the
# sidebar, the narration-script link) is unrelated code and stays same-tab.
NEW_TAB_ATTRS = ' target="_blank" rel="noopener noreferrer"'


def replace_link(match: re.Match[str], current_relpath: str) -> str:
    label = match.group(1)
    href = match.group(2)

    if href.startswith("#"):
        # A same-page anchor -- a new tab would just show the top of a
        # duplicate page instead of jumping to the anchor.
        return f'<a href="{html.escape(href, quote=True)}">{label}</a>'

    if href.startswith(("http://", "https://")):
        return f'<a href="{html.escape(href, quote=True)}"{NEW_TAB_ATTRS}>{label}</a>'

    path_part, _, fragment = href.partition("#")
    basename = Path(path_part).name

    if path_part.endswith(".md") and basename in RENDERED_BASENAMES:
        # Another page in THIS rendered set -- point at its real sibling
        # .html file (which may live in a different lesson folder), keeping
        # any #fragment.
        target_markdown_relpath = "README.md" if basename == "README.md" else next(
            name for name, _ in LESSONS if Path(name).name == basename
        )
        rewritten = relative_href(
            output_relpath(current_relpath), output_relpath(target_markdown_relpath)
        )
        if fragment:
            rewritten += f"#{fragment}"
        return f'<a href="{html.escape(rewritten, quote=True)}"{NEW_TAB_ATTRS}>{label}</a>'

    # Everything else -- an external reference doc (docs/reference/,
    # DEVELOPER_GUIDE.md, ...) or a same-folder source file
    # (lesson_NN_*.cpp) -- needs no rebasing at all: the rendered .html
    # lives in the exact same folder as the .md it was rendered from, so
    # every other relative link in that .md already resolves correctly.
    return f'<a href="{html.escape(href, quote=True)}"{NEW_TAB_ATTRS}>{label}</a>'


def render_markdown(markdown: str, current_relpath: str) -> str:
    lines = markdown.splitlines()
    parts: list[str] = []
    paragraph: list[str] = []
    blockquote_lines: list[str] = []
    list_item_lines: list[str] = []
    in_code = False
    code_lang = ""
    code_lines: list[str] = []
    in_list = False
    in_ordered_list = False
    skipped_document_title = False

    def flush_paragraph() -> None:
        nonlocal paragraph
        if paragraph:
            parts.append(f"<p>{inline_markup(' '.join(paragraph), current_relpath)}</p>")
            paragraph = []

    def flush_blockquote() -> None:
        nonlocal blockquote_lines
        if blockquote_lines:
            parts.append(
                f"<blockquote><p>{inline_markup(' '.join(blockquote_lines), current_relpath)}</p></blockquote>"
            )
            blockquote_lines = []

    def close_list_item() -> None:
        # A markdown list item's text often soft-wraps across several
        # source lines, each continuation indented with no `-`/`1.`
        # marker of its own -- those all belong to ONE <li>, not one
        # stray <p> per line.
        nonlocal list_item_lines
        if list_item_lines:
            parts.append(f"<li>{inline_markup(' '.join(list_item_lines), current_relpath)}</li>")
            list_item_lines = []

    def close_list() -> None:
        nonlocal in_list, in_ordered_list
        close_list_item()
        if in_list:
            parts.append("</ol>" if in_ordered_list else "</ul>")
            in_list = False
            in_ordered_list = False

    for line in lines:
        if line.startswith("```"):
            if in_code:
                code = "\n".join(code_lines)
                class_attr = f' class="language-{html.escape(code_lang)}"' if code_lang else ""
                parts.append(f"<pre><code{class_attr}>{html.escape(code)}</code></pre>")
                in_code = False
                code_lang = ""
                code_lines = []
            else:
                flush_paragraph()
                flush_blockquote()
                close_list()
                in_code = True
                code_lang = line[3:].strip()
            continue

        if in_code:
            code_lines.append(line)
            continue

        if not line.strip():
            flush_paragraph()
            flush_blockquote()
            close_list()
            continue

        blockquote = re.match(r"^>\s?(.*)$", line)
        if blockquote:
            # Same soft-wrap concern as list items: consecutive `> ` lines
            # are one blockquote paragraph, not one <blockquote> each.
            flush_paragraph()
            close_list()
            blockquote_lines.append(blockquote.group(1))
            continue
        if blockquote_lines:
            flush_blockquote()

        heading = re.match(r"^(#{1,3})\s+(.+)$", line)
        if heading:
            flush_paragraph()
            close_list()
            level = len(heading.group(1))
            if level == 1 and not skipped_document_title:
                skipped_document_title = True
                continue
            parts.append(
                f"<h{level}>{inline_markup(heading.group(2), current_relpath)}</h{level}>"
            )
            continue

        ordered = re.match(r"^\d+\.\s+(.+)$", line)
        unordered = re.match(r"^[-*]\s+(.+)$", line)
        if ordered or unordered:
            flush_paragraph()
            ordered_item = ordered is not None
            if not in_list or in_ordered_list != ordered_item:
                close_list()
                parts.append("<ol>" if ordered_item else "<ul>")
                in_list = True
                in_ordered_list = ordered_item
            else:
                close_list_item()
            list_item_lines = [ordered.group(1) if ordered else unordered.group(1)]
            continue

        if in_list:
            list_item_lines.append(line.strip())
        else:
            paragraph.append(line.strip())

    flush_paragraph()
    flush_blockquote()
    close_list()
    return "\n".join(parts)


def page_title(markdown: str, fallback: str) -> str:
    for line in markdown.splitlines():
        if line.startswith("# "):
            return line[2:].strip()
    return fallback


def nav_link(label: str, href: str | None) -> str:
    if href is None:
        return f'<span class="nav-button nav-disabled">{html.escape(label)}</span>'
    return f'<a class="nav-button" href="{html.escape(href, quote=True)}">{html.escape(label)}</a>'


def lesson_sidebar(current_relpath: str) -> str:
    items = []
    current_output = output_relpath(current_relpath)
    for markdown_relpath, label in LESSONS:
        href = relative_href(current_output, output_relpath(markdown_relpath))
        current = ' aria-current="page"' if markdown_relpath == current_relpath else ""
        items.append(f'<li><a href="{href}"{current}>{html.escape(label)}</a></li>')
    return "\n".join(items)


def hero_intro(title: str) -> str:
    if title == "Building an Employee Directory with Ring0":
        return "A project-based C++ course: one real application, twenty chained lessons, one persistent file."
    return "Learn one capability, run the lesson's own binary, then carry the idea into the next lesson."


def fallback_notes(markdown_relpath: str) -> dict[str, object]:
    source = SOURCE_DIR / markdown_relpath
    markdown = source.read_text(encoding="utf-8")
    title = page_title(markdown, Path(markdown_relpath).stem)
    return {
        "context": (
            f"{title} extends the employee-directory course with one of the product "
            "capabilities that sits outside the first application-building arc."
        ),
        "goal": (
            "Run the lesson's own source file, connect the result to the relevant "
            "reference or operational document, and carry that understanding into the next lesson."
        ),
        "outcomes": [
            "Identify the product capability this lesson exercises.",
            "Explain which artifact or command proves the concept.",
            "Know which reference document to use when applying it outside the tutorial.",
        ],
        "narration": (
            f"{title} continues the same course structure: one folder, one lesson document, "
            "one compiling source file, and one concrete product capability. The code is small "
            "on purpose; the important part is learning which durable artifact, command, or API "
            "surface proves the behavior."
        ),
    }


def lesson_notes(markdown_relpath: str) -> dict[str, object]:
    return LESSON_NOTES.get(notes_key(markdown_relpath), fallback_notes(markdown_relpath))


def concept_deep_dive(markdown_relpath: str) -> str:
    key = notes_key(markdown_relpath)
    if key in CONCEPT_DEEP_DIVE:
        return CONCEPT_DEEP_DIVE[key]
    title = page_title((SOURCE_DIR / markdown_relpath).read_text(encoding="utf-8"),
                       Path(markdown_relpath).stem)
    return (
        f"{title} is part of the bridge from tutorial code to operational use. "
        "The lesson keeps the example concrete, but the deeper habit is to connect every "
        "feature to the artifact that proves it: a page inventory, a baseline id, a "
        "version negotiation, a replication command, or a repeatable test result."
    )


def audio_panel(markdown_relpath: str) -> str:
    current_output = output_relpath(markdown_relpath)
    audio_href = relative_href(current_output, audio_relpath(markdown_relpath))
    script_href = relative_href(current_output, narration_relpath(markdown_relpath))
    return f"""
        <section class="audio-panel">
          <div>
            <p class="audio-label">Instructor Audio</p>
            <p class="audio-copy">Place your generated narration file at <code>{audio_href}</code>, then use the player below.</p>
          </div>
          <audio controls preload="none" src="{audio_href}"></audio>
          <a class="script-link" href="{script_href}" target="_blank" rel="noopener noreferrer">Open narration script</a>
        </section>
"""


def rich_narration(markdown_relpath: str) -> str:
    notes = lesson_notes(markdown_relpath)
    outcomes = " ".join(notes["outcomes"])
    return (
        notes["narration"].strip()
        + "\n\n"
        + concept_deep_dive(markdown_relpath)
        + "\n\nThe context for this lesson is that "
        + notes["context"]
        + "\n\nWhat we are trying to achieve is this: "
        + notes["goal"]
        + "\n\nBy the end of this lesson, the learner should be able to "
        + outcomes[0].lower()
        + outcomes[1:]
        + "\n"
    )


def explanation_panel(markdown_relpath: str) -> str:
    notes = lesson_notes(markdown_relpath)
    outcomes = " ".join(notes["outcomes"])
    return f"""
        <section class="explanation-panel">
          <p class="eyebrow-dark">Context and Intent</p>
          <h2>Before You Run the Lesson</h2>
          <p>{html.escape(notes["context"])}</p>
          <h3>What We Are Trying To Achieve</h3>
          <p>{html.escape(notes["goal"])}</p>
          <h3>Learning Outcomes</h3>
          <p>{html.escape(outcomes)}</p>
          <h3>Conceptual Layer</h3>
          <p>{html.escape(concept_deep_dive(markdown_relpath))}</p>
        </section>
"""


def instructor_narrative_panel(markdown_relpath: str) -> str:
    notes = lesson_notes(markdown_relpath)
    paragraphs = "\n".join(
        f"<p>{html.escape(paragraph.strip())}</p>"
        for paragraph in notes["narration"].strip().split("\n\n")
    )
    return f"""
        <section class="narrative-panel">
          <p class="eyebrow-dark">Instructor Narrative</p>
          <h2>What This Lesson Means</h2>
          {paragraphs}
        </section>
"""


def wrap_page(
    title: str,
    body: str,
    previous_relpath: str | None,
    next_relpath: str | None,
    markdown_relpath: str,
) -> str:
    current_output = output_relpath(markdown_relpath)
    index_href = relative_href(current_output, output_relpath("README.md"))
    previous_href = (
        relative_href(current_output, output_relpath(previous_relpath)) if previous_relpath else None
    )
    next_href = (
        relative_href(current_output, output_relpath(next_relpath)) if next_relpath else None
    )
    css_href = relative_href(current_output, "assets/training.css")
    logo_href = relative_href(current_output, "assets/ring0-logo.svg")
    nav = (
        f'<nav class="topnav">'
        f'{nav_link("Previous", previous_href)}'
        f'{nav_link("Index", index_href)}'
        f'{nav_link("Next", next_href)}'
        f"</nav>"
    )
    bottom_nav = nav.replace('class="topnav"', 'class="bottomnav"')
    sidebar = lesson_sidebar(markdown_relpath)
    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{html.escape(title)} - Ring0 Training</title>
  <link rel="stylesheet" href="{css_href}">
</head>
<body>
  <main class="page">
    <header class="brandbar">
      <a class="brand" href="{index_href}">
        <img src="{logo_href}" alt="Ring0 Training logo">
        <span>
          <span class="brand-name">Ring0 Training</span>
          <span class="brand-subtitle">Employee directory, lesson by lesson</span>
        </span>
      </a>
      {nav}
    </header>
    <div class="layout">
      <aside class="sidebar">
        <p class="sidebar-title">Course Lessons</p>
        <ol class="lesson-list">
          {sidebar}
        </ol>
      </aside>
      <article class="content">
        <section class="hero">
          <p class="eyebrow">Ring0 Developer Course</p>
          <h1>{html.escape(title)}</h1>
          <p>{html.escape(hero_intro(title))}</p>
        </section>
        {audio_panel(markdown_relpath)}
        {explanation_panel(markdown_relpath)}
        {instructor_narrative_panel(markdown_relpath)}
        {body}
        {bottom_nav}
      </article>
    </div>
  </main>
</body>
</html>
"""


def main() -> None:
    SOURCE_DIR.mkdir(parents=True, exist_ok=True)
    ASSET_OUTPUT.mkdir(parents=True, exist_ok=True)
    for asset in ASSET_SOURCE.iterdir():
        if asset.is_file():
            shutil.copy2(asset, ASSET_OUTPUT / asset.name)

    for index, (markdown_relpath, fallback_title) in enumerate(LESSONS):
        source = SOURCE_DIR / markdown_relpath
        markdown = source.read_text(encoding="utf-8")
        title = page_title(markdown, fallback_title)
        previous_relpath = LESSONS[index - 1][0] if index > 0 else None
        next_relpath = LESSONS[index + 1][0] if index + 1 < len(LESSONS) else None
        body = render_markdown(markdown, markdown_relpath)
        output = SOURCE_DIR / output_relpath(markdown_relpath)
        output.parent.mkdir(parents=True, exist_ok=True)
        narration_path = SOURCE_DIR / narration_relpath(markdown_relpath)
        narration_path.write_text(rich_narration(markdown_relpath), encoding="utf-8")
        output.write_text(
            wrap_page(title, body, previous_relpath, next_relpath, markdown_relpath),
            encoding="utf-8",
        )

    print(f"Rendered {len(LESSONS)} training pages, each into its own lesson folder under {SOURCE_DIR}")
    print("Narration scripts (text only, no audio) written alongside each lesson's own .html")


if __name__ == "__main__":
    main()
