#!/usr/bin/env python3
"""Render the English Ring0 training Markdown files as static HTML."""

from __future__ import annotations

import html
import re
import shutil
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIR = ROOT / "training" / "en"
OUTPUT_DIR = ROOT / "training" / "html"
ASSET_SOURCE = ROOT / "training" / "assets"
ASSET_OUTPUT = OUTPUT_DIR / "assets"

LESSONS = [
    ("README.md", "Index"),
    ("00-version-compatibility.md", "Phase 00"),
    ("01-bind-type.md", "Phase 01"),
    ("02-persist-reopen.md", "Phase 02"),
    ("03-handle-update.md", "Phase 03"),
    ("04-relationships.md", "Phase 04"),
    ("05-transactions-recovery.md", "Phase 05"),
    ("06-snapshots.md", "Phase 06"),
    ("07-streaming-query.md", "Phase 07"),
    ("08-server-connection.md", "Phase 08"),
    ("09-remote-operation.md", "Phase 09"),
    ("10-handshake-capabilities.md", "Phase 10"),
    ("11-remote-facade.md", "Phase 11"),
    ("12-graph-traversal.md", "Phase 12"),
    ("13-async-io.md", "Phase 13"),
]

NARRATION = {
    "README.md": """Welcome to the Ring0 Server Training.

In this course, you will move step by step from a tiny C++ compatibility check to a networked server API with remote operations, facades, graph traversal, and asynchronous I/O.

You already know C++, so this training will not spend time explaining structs, lambdas, templates, or RAII. Instead, we will use those tools to understand Ring0's programming model.

As you go through the lessons, pay attention to the pattern: define a small domain type, bind it to the catalog, store it transactionally, reopen it, query it, expose it through a server, and finally call higher-level remote APIs.

By the end, you should understand how the phase examples fit together as one incremental story, not as isolated demos.""",
    "00-version-compatibility.md": """In this lesson, you should learn how Ring0 starts with an explicit compatibility contract.

The example does not open a database and does not start a server. It simply compares a client protocol version with a server protocol version and asks the library to negotiate a compatible result.

For a C++ developer, think of this as a small capability gate before using an API. The important habit is this: clients should not assume that the server speaks the exact protocol they expect.

After this lesson, you should be able to explain why protocol negotiation belongs at the beginning of any client and server interaction.""",
    "01-bind-type.md": """In this lesson, you should learn how a plain C++ type becomes known to Ring0.

The Customer struct is ordinary C++. The interesting part is BindingBuilder, which maps stable field ids to actual C++ data members.

The field ids are the durable contract. The names are helpful, but the ids are what let Ring0 decode persisted objects safely over time.

After this lesson, you should understand that Ring0 does not require a special base class for your domain object. You describe your type at the boundary, then the catalog can track it.""",
    "02-persist-reopen.md": """In this lesson, you should learn how Ring0 persists an object and reads it after reopening the database.

The example creates one Customer, commits the transaction, stores the ObjectId, then closes that database lifetime. It opens the file again and materializes the object by its logical identity.

The important point is that persistence is not proven by reading an object that is still in memory. Persistence is proven by reopening the file and decoding it again.

After this lesson, you should understand the role of ObjectId and why the C++ binding must be registered again when a process wants to materialize typed objects.""",
    "03-handle-update.md": """In this lesson, you should learn how to update a typed object through Handle<T>.

The example creates an Account, then updates only the balance field using a pointer-to-member expression.

This is a very C++-native API: the field is selected through the type system, not through a string at the update site.

The update still happens inside a transaction. The handle gives you typed access, but it does not bypass durability or transaction rules.

After this lesson, you should be comfortable with the idea that Ring0 handles are typed object references used under transactional control.""",
    "04-relationships.md": """In this lesson, you should learn how Ring0 stores relationships between objects.

The Employee type has a Ref to Department and an OwnedRef to Badge. Both are stored as logical object references, but they mean different things.

Ref is an association. OwnedRef represents ownership. That distinction matters when the domain model needs lifecycle rules.

The example prints the referenced object ids so you can see that relationships are durable edges, not embedded copies.

After this lesson, you should understand the difference between associating with another object and owning another object.""",
    "05-transactions-recovery.md": """In this lesson, you should learn the minimum durable transaction story.

The example writes an Account, commits it, ends that database lifetime, reopens the file, and reads the account back.

The important thing is the reopen. It proves that the committed data is not just visible in process memory.

This lesson is intentionally small. Crash recovery and failpoints can be more complex, but the application contract starts here: committed data survives reopen.

After this lesson, you should be able to describe why commit and reopen are the simplest useful durability check.""",
    "06-snapshots.md": """In this lesson, you should learn what a stable read view means.

The example commits an account with balance one hundred, opens a snapshot, then commits a later transaction changing the balance to two hundred.

The snapshot read still sees one hundred. The current read sees two hundred.

This does not mean Ring0 copied the whole database. It means the object layer can read the version that belongs to the snapshot's epoch.

After this lesson, you should understand why snapshots are useful when readers need consistency while writers keep moving forward.""",
    "07-streaming-query.md": """In this lesson, you should learn how typed queries stream results.

The example creates several Item objects, filters for even values, limits the result to two, and then consumes the stream one result at a time.

If you know C++ ranges, this should feel familiar. But unlike a simple in-memory range, a database stream can report an error after some rows have already been delivered.

After this lesson, you should understand why streaming improves time to first result and why callers must check every streamed Result.""",
    "08-server-connection.md": """In this lesson, you should learn how the local object model becomes reachable through a server.

The example seeds a database, starts a loopback server on an operating-system-selected port, and uses ServerConnection to connect as an application client.

The server handles one session in a background thread. The client sends a QueryDescription using a type id and receives object payloads.

The important transition is that the client is no longer running C++ callbacks inside the database process. It is talking to a server through a protocol.

After this lesson, you should understand the basic shape of a Ring0 client and server interaction.""",
    "09-remote-operation.md": """In this lesson, you should learn how to call domain behavior remotely.

The example registers the TransferFunds operation in an OperationRegistry, admits the module manifest hash, loads the module, and attaches the registry to the server.

The client encodes operation arguments and calls the operation by id. The server owns the transaction boundary and applies the domain operation.

This is an important architectural shift. The client sends intent. The server performs the mutation under its own rules.

After this lesson, you should understand why remote operations are safer than exposing raw storage mutation to clients.""",
    "10-handshake-capabilities.md": """In this lesson, you should learn how to inspect public server capabilities.

The example starts a server and performs only the handshake. It reads the negotiated protocol version and public limits such as max concurrent streams.

This is the network version of the compatibility habit from phase zero.

Before a client depends on advanced behavior, it can ask what the server supports.

After this lesson, you should understand how handshake metadata helps clients adapt to server capabilities.""",
    "11-remote-facade.md": """In this lesson, you should learn how facades improve the remote API.

Phase nine called an operation directly by id. Phase eleven opens a typed AccountsFacade and invokes TransferFunds through that handle.

The facade does not replace operations. It organizes and versions the public surface that consumers see.

For application developers, this is a cleaner API: open a facade, get a typed handle, and invoke a typed method.

After this lesson, you should understand that facades are about consumer ergonomics and compatibility, while operation dispatch still performs the work.""",
    "12-graph-traversal.md": """In this lesson, you should learn the graph traversal contract.

The example uses an in-memory map so the idea is easy to see. The traversal function only needs a starting ObjectId and an adjacency callback.

In a real database-backed application, that callback would resolve outgoing edges from persisted objects.

The BFS result is streamed, so callers can process traversal items incrementally.

After this lesson, you should understand that Ring0 graph algorithms are built around object ids and adjacency resolution, not a single mandatory graph container.""",
    "13-async-io.md": """In this lesson, you should learn the phase thirteen asynchronous I/O model.

The example opens an AsyncFile, requests the native async backend, submits positional writes and a sync operation, and then waits at a barrier.

The essential concept is separation between submission and completion. submit_write_at queues work. barrier waits for all previously submitted work to finish.

This pattern is important for storage engines because it allows multiple I/O operations to be in flight while still giving the caller an explicit durability point.

After this lesson, you should understand why asynchronous I/O is not just faster file writing. It is a control model for ordering, concurrency, and durability.""",
}

LESSON_NOTES = {
    "README.md": {
        "context": (
            "This course is the guided path through Ring0 as a product, not just a list of "
            "sample programs. The examples are small on purpose, but the architecture they "
            "introduce is the same architecture an application would use when it grows from "
            "embedded storage to a networked service."
        ),
        "goal": (
            "Set expectations for the full learning arc: compatibility, object binding, "
            "persistence, transactions, snapshots, streaming, server access, remote behavior, "
            "facades, graph traversal, and asynchronous I/O."
        ),
        "outcomes": [
            "Understand why the course starts with contracts before storage.",
            "See how each phase adds one capability without discarding the previous model.",
            "Know where the source examples live and how to build them.",
        ],
        "narration": """Think of this training as a guided technical onboarding for a commercial C++ storage product.

The examples are intentionally compact, but they are not toy ideas. Each one isolates a production concern: version negotiation, schema description, durable identity, transactional mutation, server boundaries, remote domain behavior, and asynchronous persistence.

What we want to achieve is not memorizing commands. We want to build a mental model. By the end of the course, a developer should be able to look at a Ring0 application and understand where type metadata is registered, where object identity is created, where transactions begin and end, where the server takes ownership of remote calls, and where low-level I/O becomes explicitly ordered.

The course assumes C++ fluency so we can move quickly. When we see a struct, lambda, smart pointer, or pointer-to-member expression, we will treat it as normal C++ and focus on what Ring0 adds around it.

Use the examples as executable checkpoints. Run the code, read the output, then return to the lesson and ask: what contract did this phase add to the system? That question is the spine of the training.""",
    },
    "00-version-compatibility.md": {
        "context": (
            "Any serious client/server system needs a compatibility boundary before it exchanges "
            "stateful data. Phase 00 introduces that boundary before storage or networking makes "
            "the situation more complex."
        ),
        "goal": (
            "Learn how Ring0 represents protocol versions and why negotiation is the first "
            "decision a client should make."
        ),
        "outcomes": [
            "Explain the difference between project version and protocol version.",
            "Describe what a successful negotiation permits the caller to do next.",
            "Recognize incompatible protocol versions as a normal operational case.",
        ],
        "narration": """This lesson is deliberately small because the concept is foundational.

Before we talk about pages, objects, transactions, or servers, we need a compatibility story. A client that connects to a server must know whether both sides agree on the public protocol. If that agreement is missing, every later feature becomes unsafe.

The example creates a client version and a server version, then asks Ring0 to negotiate. In a production system this check would happen before the client sends richer requests. It is not glamorous, but it is the kind of quiet contract that prevents expensive failures later.

The project version tells you what build of the product you are running. The protocol version tells you what wire-level conversation is allowed. Those are related, but they are not the same thing.

What we want to achieve in this phase is a habit: do not assume compatibility. Ask for it, handle failure, and only then continue. Every networked lesson later in the course builds on this discipline.""",
    },
    "01-bind-type.md": {
        "context": (
            "Ring0 stores objects, but it does not force domain types to inherit from a framework "
            "base class. Instead, C++ types are described at the boundary through bindings."
        ),
        "goal": (
            "Understand how a plain C++ struct becomes a persistent catalog type with stable "
            "field identifiers."
        ),
        "outcomes": [
            "Read a BindingBuilder definition and identify the persistent field ids.",
            "Explain why field ids matter more than field names for durability.",
            "Understand why binding is required before typed object operations.",
        ],
        "narration": """This phase is where ordinary C++ meets the Ring0 catalog.

The Customer struct has no framework inheritance and no hidden macro. It is just a domain type with a name and a score. The binding is the bridge between that C++ type and the persistent schema stored by Ring0.

The central idea is stable field identity. When the binding says field one is name and field two is score, those ids become part of the durable contract. A name can help humans understand the schema, but a stable id helps software decode old data predictably.

What we want to achieve is confidence that Ring0 can work with normal C++ shapes. You describe the type explicitly, register it, and then the catalog can assign and track a type id.

This is the first step toward persistence. We are not yet storing a Customer object. We are teaching the database what a Customer means. That distinction matters: schema knowledge comes before durable object instances.""",
    },
    "02-persist-reopen.md": {
        "context": (
            "After a type is known to the catalog, the next question is whether an object can "
            "survive beyond the current process lifetime."
        ),
        "goal": (
            "Persist a typed object, keep its ObjectId, reopen the database, and materialize the "
            "same logical object again."
        ),
        "outcomes": [
            "Understand ObjectId as logical identity rather than a physical location.",
            "See why reopening is a stronger persistence proof than an immediate read.",
            "Know why bindings must be registered again before typed materialization.",
        ],
        "narration": """This lesson turns the schema from phase one into an actual durable object.

The example creates a Customer, commits it, stores the ObjectId, and then lets that first database lifetime end. That lifetime boundary is important. If we only read the object immediately after writing it, we might accidentally be validating in-memory state. Reopening the file forces the system to prove that the object was encoded, stored, found, and decoded again.

The ObjectId is the application-level identity. It is not asking you to care about the page number, slot number, or storage layout. Ring0 owns those details. Your domain code keeps the logical identity.

Notice that the binding is registered again after reopen. The database file contains persistent data and metadata, but the running process still needs the C++ mapping to turn bytes back into a Customer instance.

What we want to achieve in this phase is the first complete persistence loop: bind, create, commit, reopen, materialize. Most later examples repeat this loop with more advanced behavior layered on top.""",
    },
    "03-handle-update.md": {
        "context": (
            "Real applications need to change existing objects. Phase 03 introduces typed mutation "
            "through Handle<T> while keeping transaction boundaries explicit."
        ),
        "goal": (
            "Update one persistent field using a C++ pointer-to-member expression and commit the "
            "change."
        ),
        "outcomes": [
            "Understand Handle<T> as a typed reference to a persistent object.",
            "See how member pointers select persistent fields without string lookup.",
            "Connect typed mutation with transaction commit semantics.",
        ],
        "narration": """This phase introduces typed updates, and it is a good example of Ring0 trying to feel natural to C++ developers.

The Account object has an owner and a balance. After creating it, the example updates the balance through Handle<T>. The field is selected using a pointer-to-member expression, not a string. That means the compiler participates in the API.

This is convenient, but it is still a database operation. The handle does not give permission to mutate durable state outside a transaction. The transaction remains the unit that decides whether the update becomes durable.

What we want to achieve is a clean mental model: Handle<T> gives typed access to a logical object, while the transaction gives durability and atomicity. Those two concepts work together.

As systems grow, this pattern matters because it keeps domain code readable. You can see which field is being changed, and you can see where the commit happens.""",
    },
    "04-relationships.md": {
        "context": (
            "Object databases are valuable when domain models contain relationships. Phase 04 "
            "shows how Ring0 stores durable edges between objects."
        ),
        "goal": (
            "Represent association and ownership using Ref<T> and OwnedRef<T>."
        ),
        "outcomes": [
            "Distinguish association from ownership in persisted object graphs.",
            "Understand relationships as logical ObjectId edges.",
            "Recognize why relationship semantics matter for lifecycle rules.",
        ],
        "narration": """This phase moves from isolated objects to a small object graph.

The Employee object refers to a Department and owns a Badge. Those two relationships look similar in C++ because both point at another object, but they mean different things in the domain model.

A Ref is an association. It says this object is connected to another object, but it does not own that object's lifecycle. An OwnedRef communicates a stronger relationship: the target belongs to the owner.

The example prints object ids because the relationship is stored as a durable edge, not as an embedded copy. That is an important distinction. If the target object changes, the relationship still points to the target identity.

What we want to achieve is the ability to model a realistic domain without flattening everything into one record. Later graph-oriented features build on this same idea of stable object identity and resolvable edges.""",
    },
    "05-transactions-recovery.md": {
        "context": (
            "Persistence is useful only if committed data survives process boundaries and recovery "
            "events. Phase 05 introduces the practical durability checkpoint."
        ),
        "goal": (
            "Commit an object, reopen the database, and verify that the durable state is still "
            "available."
        ),
        "outcomes": [
            "Connect commit with durable visibility after reopen.",
            "Understand why a reopen validates more than an in-process read.",
            "Prepare for WAL and recovery behavior used by later phases.",
        ],
        "narration": """This lesson is about trust.

When a database says a transaction committed, the application expects the data to survive beyond the current object in memory. The example writes an Account, commits it, closes that lifetime, reopens the database, and reads the Account again.

This is not a crash simulation yet. It is the simpler guarantee that committed data is durable enough to be found after a fresh open.

The write-ahead log and recovery machinery are deeper topics, but the application-facing contract is easy to state: if commit succeeds, later opens should see the committed object.

What we want to achieve is a durable checkpoint in the course. From this point on, when later examples talk about snapshots, remote operations, or facades, they are building on a storage layer that already has a transaction and recovery story.""",
    },
    "06-snapshots.md": {
        "context": (
            "Readers and writers often overlap. Phase 06 introduces stable read views so readers "
            "can observe a consistent version while newer commits continue."
        ),
        "goal": (
            "Show one snapshot reading the old object version while a current read sees the new "
            "version."
        ),
        "outcomes": [
            "Explain why a snapshot is a read view, not a full database copy.",
            "Understand how old and current versions can both be meaningful.",
            "Recognize the role of snapshots in concurrent applications.",
        ],
        "narration": """This phase introduces one of the most important ideas for concurrent data systems: a stable read view.

The example starts with an Account balance of one hundred. It opens a snapshot, then updates the current balance to two hundred in a later transaction. After that, two reads produce different answers, and both answers are correct.

The snapshot sees the world as it existed when the snapshot was opened. The current read sees the latest committed state.

This does not mean Ring0 copied the entire database. The snapshot is a logical view over object versions. That is what makes it practical.

What we want to achieve is an intuition for MVCC-style behavior. A reader can keep working with a consistent view, while writers continue to commit newer versions. Later, when remote queries and graph traversal appear, this same consistency idea becomes even more valuable.""",
    },
    "07-streaming-query.md": {
        "context": (
            "Applications should not always wait for a full result set before doing useful work. "
            "Phase 07 introduces lazy, typed query streaming."
        ),
        "goal": (
            "Filter typed objects, limit the result set, and consume results incrementally."
        ),
        "outcomes": [
            "Understand query<T>() as a typed query entry point.",
            "See why streaming improves time to first result.",
            "Remember that each streamed item can carry either a value or an error.",
        ],
        "narration": """This phase shifts from direct object lookup to query processing.

The example seeds several Item objects and then asks for even values, limited to two results. That may look like a small C++ range pipeline, and that is intentional. The API should feel familiar.

But the database meaning is important. A streaming query does not require the whole result set to be materialized before the caller sees the first row. That can improve time to first result and reduce memory pressure.

Each streamed result is still a Result type. The caller must check it. In a real server stream, an error might arrive after some rows were already delivered.

What we want to achieve is a mental model of queries as incremental flows. This prepares you for the network streaming example, where incremental delivery becomes part of the client/server protocol.""",
    },
    "08-server-connection.md": {
        "context": (
            "Phase 08 crosses the process boundary. The database is now exposed through a server, "
            "and applications connect through a client library."
        ),
        "goal": (
            "Start a loopback server, connect with ServerConnection, and collect remote query "
            "results."
        ),
        "outcomes": [
            "Identify the seed, listen, serve, connect, and collect steps.",
            "Understand why the remote query uses QueryDescription and type ids.",
            "See how the application client avoids direct access to server internals.",
        ],
        "narration": """This is the point where the course becomes a server course, not only an embedded storage course.

The example first seeds a database locally. Then it starts a server on loopback, using port zero so the operating system can choose a free port. The server handles one session in a background thread.

The client uses ServerConnection, which packages the handshake and client operations into a smaller application-facing API. It sends a QueryDescription to the server and collects the objects returned by the remote stream.

The important architectural shift is that the client is no longer running arbitrary C++ code inside the database. It is describing a request across a protocol boundary.

What we want to achieve is a clear picture of client/server responsibility. The server owns the database. The client connects, negotiates, sends a structured request, and receives structured results.""",
    },
    "09-remote-operation.md": {
        "context": (
            "Querying data is not enough for many applications. Phase 09 moves domain behavior to "
            "the server through registered operations."
        ),
        "goal": (
            "Register TransferFunds, call it remotely, and let the server perform the mutation "
            "transactionally."
        ),
        "outcomes": [
            "Understand OperationRegistry as the dispatch table for remote behavior.",
            "See why module manifests and admitted hashes matter.",
            "Recognize that clients send intent while the server owns mutation.",
        ],
        "narration": """This phase adds server-side behavior.

The example uses TransferFunds because it is a familiar domain operation. Moving money is not just a raw write. It has rules: debit one account, credit another account, and keep the operation atomic.

The server registers the operation in an OperationRegistry. The module manifest is admitted before loading, modeling the allowlist discipline that a production system needs. Then the client encodes arguments and calls the operation by id.

The key architectural point is ownership. The client does not directly edit account balances in server storage. It asks the server to perform a named operation. The server validates and commits the behavior under its own transaction boundary.

What we want to achieve is a safer remote API model. Remote operations let you expose business actions without exposing arbitrary storage mutation.""",
    },
    "10-handshake-capabilities.md": {
        "context": (
            "A robust client should learn what a server supports before relying on optional limits "
            "or advanced behavior."
        ),
        "goal": (
            "Use the handshake to inspect negotiated protocol version and public server limits."
        ),
        "outcomes": [
            "Read protocol and capability values returned by the handshake.",
            "Connect phase 10 back to phase 00 compatibility negotiation.",
            "Understand why capability discovery supports safer clients.",
        ],
        "narration": """This phase returns to the idea of contracts, now through the network.

The example starts a server and performs only the handshake. It does not query objects and does not call a domain operation. That is the point: sometimes the first useful thing a client can do is ask what the server supports.

The returned information includes protocol version and public limits such as maximum concurrent streams. A production client can use that information to decide how aggressively to stream, whether to enable an optional path, or whether to reject the connection.

This is the same habit you saw in phase zero, but now it is attached to a live server.

What we want to achieve is an operational mindset. Compatibility and capability checks are not ceremony. They are how clients stay predictable as servers evolve.""",
    },
    "11-remote-facade.md": {
        "context": (
            "Operations are powerful, but consumers often need a stable, typed surface. Phase 11 "
            "introduces facades as that public API layer."
        ),
        "goal": (
            "Open a typed AccountsFacade remotely and invoke TransferFunds through the facade "
            "handle."
        ),
        "outcomes": [
            "Distinguish operation dispatch from facade discovery.",
            "Understand facades as versioned consumer-facing surfaces.",
            "See how typed invocation improves ergonomics over raw operation ids.",
        ],
        "narration": """This phase improves the consumer experience.

In phase nine, the client called an operation directly. That works, but larger applications need a more organized public surface. A facade groups related methods and gives clients a stable handle to that surface.

The server registers both the operation registry and the facade catalog. The client opens AccountsFacade and then invokes TransferFunds through the typed handle.

Under the hood, the work still travels through operation dispatch. The facade does not replace operations. It gives consumers a clearer, versioned API.

What we want to achieve is the shape of an application SDK. Instead of making every consumer remember operation ids and argument encoding details, the facade gives them a typed C++ entry point with compatibility checks around it.""",
    },
    "12-graph-traversal.md": {
        "context": (
            "Object relationships naturally form graphs. Phase 12 introduces traversal as a "
            "reusable algorithm over ObjectId adjacency."
        ),
        "goal": (
            "Run breadth-first traversal using an adjacency callback, then relate that callback "
            "to persisted graph edges."
        ),
        "outcomes": [
            "Understand why traversal depends on ObjectId and adjacency, not one fixed container.",
            "See BFS as a streaming result producer.",
            "Know how a database-backed adjacency callback would replace the in-memory map.",
        ],
        "narration": """This phase teaches graph traversal by stripping away storage details first.

The example uses an in-memory map from ObjectId to neighboring ObjectIds. That keeps the traversal contract visible. The BFS function does not need to know where edges come from. It only needs a starting id and an adjacency callback.

In a real Ring0-backed graph, that callback would resolve outgoing references from persisted objects. The algorithm shape would remain the same.

The result is streamed, which means a caller can process visits incrementally and potentially stop early.

What we want to achieve is separation of concerns. Ring0 can store object relationships, and traversal algorithms can operate over a small adjacency abstraction. That makes graph behavior reusable instead of hard-wired to one storage representation.""",
    },
    "13-async-io.md": {
        "context": (
            "Storage engines eventually need explicit control over I/O ordering and concurrency. "
            "Phase 13 introduces asynchronous positional file operations."
        ),
        "goal": (
            "Submit writes and sync operations without waiting after each call, then wait at an "
            "explicit barrier."
        ),
        "outcomes": [
            "Distinguish I/O submission from I/O completion.",
            "Understand max_inflight as a concurrency bound.",
            "Recognize barrier as the explicit completion and ordering point.",
        ],
        "narration": """This final lesson moves below the object and server APIs into the storage engine's I/O control model.

The example opens an AsyncFile and asks for the native asynchronous backend. It submits a write at offset zero, a sync request, and another write at offset sixty-four. The important detail is that submission does not mean completion.

The barrier is where the example waits for all previously submitted work to finish. That gives the caller a clear point for ordering and durability reasoning.

The max_inflight option is also important. Asynchronous I/O is not simply about doing everything at once. A storage engine needs bounded concurrency so it can keep throughput high without losing control.

What we want to achieve is a practical understanding of asynchronous I/O as a contract: submit work, bound the amount of outstanding work, and choose the exact moment where completion is required.""",
    },
}

CONCEPT_DEEP_DIVE = {
    "README.md": """The larger concept behind the course is progressive disclosure of a storage system. A database is not one feature; it is a stack of agreements. At the bottom are compatibility and file I/O. Above that are schemas, object identity, transactions, and versioned reads. Above that are query streams, server protocols, operations, and facades. The training follows that stack so the learner can see why each layer exists before depending on it.""",
    "00-version-compatibility.md": """Compatibility is a product design problem as much as a technical one. Once an application depends on a database server, upgrades become normal. A protocol version gives both sides a way to decide whether they can communicate without guessing. Major versions usually represent breaking changes, while minor versions can represent compatible evolution. The habit we want is defensive optimism: try to negotiate, continue when the result is safe, and fail clearly when it is not.""",
    "01-bind-type.md": """Binding is Ring0's answer to the impedance mismatch between C++ memory layout and durable schema. A C++ struct is compiled into one program, but stored data may live for years and be opened by future versions. The binding tells Ring0 which fields are part of the persistent contract. Stable field ids matter because names and code layout can evolve; the id is the durable coordinate that lets old bytes be interpreted with intent.""",
    "02-persist-reopen.md": """Persistence is not the same as allocation. Creating an object inside a process only proves that memory was allocated and the API accepted the value. Reopening the database proves a stronger chain: the object was encoded, written, committed, indexed by identity, found again, and decoded through the registered binding. This is the first point where Ring0 starts behaving like durable infrastructure rather than an in-memory object registry.""",
    "03-handle-update.md": """A handle is a controlled reference to durable identity. In ordinary C++, a reference or pointer lets you mutate memory directly. In Ring0, a handle gives you typed access to an object, but mutation still flows through a transaction because the system must preserve atomicity, versioning, and recovery. The pointer-to-member expression makes the update feel native to C++, while the transaction keeps the database semantics intact.""",
    "04-relationships.md": """Object relationships are the reason an object database can model domains more naturally than a flat record store. A Ref expresses that one object knows another object. An OwnedRef expresses that one object has lifecycle responsibility for another object. That semantic difference becomes important when deleting, traversing, or reasoning about ownership. The relationship is stored as identity, not as a physical address, so the graph remains meaningful even when storage layout changes.""",
    "05-transactions-recovery.md": """A transaction is a promise about grouping. Either the intended change becomes visible as a coherent unit, or it does not. Recovery extends that promise across process boundaries and failures. Even though this example only performs a clean reopen, it prepares the learner to think in terms of committed intent rather than individual writes. Storage engines need this discipline because pages, indexes, identity maps, and logs may all be touched by one logical change.""",
    "06-snapshots.md": """Snapshots are a way to separate time from mutation. Without snapshots, a reader can be confused by concurrent writes because the meaning of 'the database' changes while the reader is working. With a snapshot, the reader asks for the database as of a particular logical moment. This is the core intuition behind MVCC: multiple committed versions can coexist long enough for readers and writers to make progress without blocking each other unnecessarily.""",
    "07-streaming-query.md": """Streaming is about latency and memory shape. A materialized query waits until the full answer exists before the caller can act. A streaming query allows the caller to begin work when the first result is available. That is valuable for large result sets, remote clients, and interactive applications. It also changes error handling, because a stream can succeed for a while and then fail later. The consumer must treat each item as part of an ongoing conversation.""",
    "08-server-connection.md": """Crossing the server boundary changes the programming model. Inside one process, a C++ lambda can inspect objects directly. Across a network, the client must describe intent in a serializable form. QueryDescription is that boundary object. It carries enough information for the server to execute a query without importing client code. This separation is what lets Ring0 become a service rather than only an embedded library.""",
    "09-remote-operation.md": """Remote operations move business behavior to the place where data is owned. That is important because many domain changes are not simple writes. A transfer needs validation, debit, credit, rollback on error, and a commit boundary. If clients performed raw mutations, each client would need to reimplement the rules correctly. By registering operations on the server, Ring0 lets applications expose intent-oriented commands while keeping invariants close to the data.""",
    "10-handshake-capabilities.md": """A capability handshake is how a client learns the shape of the server it has reached. This matters when systems evolve. A newer client may know optimizations or limits that an older server does not support. A server may also advertise conservative limits to protect itself. Reading those values early lets the client choose behavior instead of discovering incompatibility through failures halfway through a workflow.""",
    "11-remote-facade.md": """A facade is a product-facing API boundary. Operations are the executable units, but facades organize those units into a stable surface that clients can understand and version. This is similar to the difference between individual functions and a service interface. The facade gives a consumer a typed handle and a vocabulary of methods, while the operation layer still performs dispatch, validation, and transactional execution.""",
    "12-graph-traversal.md": """A graph is a set of nodes connected by edges. In Ring0 terms, nodes can be objects and edges can be relationships expressed through ObjectId references. Traversal algorithms such as breadth-first search answer questions about reachability and distance: what can I reach from here, and how many steps away is it. BFS explores level by level, so it is useful when the first time you reach a node should represent the shortest number of edges from the start. The example uses an in-memory adjacency map, but the concept is the same when adjacency is resolved from persisted object relationships.""",
    "13-async-io.md": """Asynchronous I/O is about control over waiting. Synchronous I/O ties submission and completion together: the caller asks for work and blocks until it is done. Asynchronous I/O separates those moments. A storage engine can submit several positional operations, keep a bounded number in flight, and then choose a barrier where completion is required. That model is useful for write-ahead logging, page flushing, and any workflow where ordering matters but waiting after every individual write would waste throughput.""",
}


def output_name(markdown_name: str) -> str:
    if markdown_name == "README.md":
        return "index.html"
    return Path(markdown_name).with_suffix(".html").name


def page_stem(markdown_name: str) -> str:
    return Path(output_name(markdown_name)).stem


def inline_markup(text: str) -> str:
    escaped = html.escape(text)
    escaped = re.sub(r"`([^`]+)`", r"<code>\1</code>", escaped)
    escaped = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", replace_link, escaped)
    return escaped


def replace_link(match: re.Match[str]) -> str:
    label = match.group(1)
    href = match.group(2)
    if href.endswith(".md"):
        href = output_name(Path(href).name)
    return f'<a href="{html.escape(href, quote=True)}">{label}</a>'


def render_markdown(markdown: str) -> str:
    lines = markdown.splitlines()
    parts: list[str] = []
    paragraph: list[str] = []
    in_code = False
    code_lang = ""
    code_lines: list[str] = []
    in_list = False
    in_ordered_list = False
    skipped_document_title = False

    def flush_paragraph() -> None:
        nonlocal paragraph
        if paragraph:
            parts.append(f"<p>{inline_markup(' '.join(paragraph))}</p>")
            paragraph = []

    def close_list() -> None:
        nonlocal in_list, in_ordered_list
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
                close_list()
                in_code = True
                code_lang = line[3:].strip()
            continue

        if in_code:
            code_lines.append(line)
            continue

        if not line.strip():
            flush_paragraph()
            close_list()
            continue

        heading = re.match(r"^(#{1,3})\s+(.+)$", line)
        if heading:
            flush_paragraph()
            close_list()
            level = len(heading.group(1))
            if level == 1 and not skipped_document_title:
                skipped_document_title = True
                continue
            parts.append(f"<h{level}>{inline_markup(heading.group(2))}</h{level}>")
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
            item_text = ordered.group(1) if ordered else unordered.group(1)
            parts.append(f"<li>{inline_markup(item_text)}</li>")
            continue

        paragraph.append(line.strip())

    flush_paragraph()
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


def lesson_sidebar(current_output: str) -> str:
    items = []
    for markdown_name, label in LESSONS:
        href = output_name(markdown_name)
        current = ' aria-current="page"' if href == current_output else ""
        items.append(f'<li><a href="{href}"{current}>{html.escape(label)}</a></li>')
    return "\n".join(items)


def hero_intro(title: str) -> str:
    if title == "Ring0 Server Training":
        return "A commercial-grade C++ learning path from local object storage to networked server APIs."
    return "Learn one capability, run the example, then carry the idea into the next phase."


def audio_panel(markdown_name: str) -> str:
    stem = page_stem(markdown_name)
    audio_href = f"audio/{stem}.mp3"
    script_href = f"narration/{stem}.txt"
    return f"""
        <section class="audio-panel">
          <div>
            <p class="audio-label">Instructor Audio</p>
            <p class="audio-copy">Place your generated narration file at <code>{audio_href}</code>, then use the player below.</p>
          </div>
          <audio controls preload="none" src="{audio_href}"></audio>
          <a class="script-link" href="{script_href}">Open narration script</a>
        </section>
"""


def rich_narration(markdown_name: str) -> str:
    notes = LESSON_NOTES[markdown_name]
    outcomes = " ".join(notes["outcomes"])
    return (
        notes["narration"].strip()
        + "\n\n"
        + CONCEPT_DEEP_DIVE[markdown_name]
        + "\n\nThe context for this lesson is that "
        + notes["context"]
        + "\n\nWhat we are trying to achieve is this: "
        + notes["goal"]
        + "\n\nBy the end of this lesson, the learner should be able to "
        + outcomes[0].lower()
        + outcomes[1:]
        + "\n"
    )


def explanation_panel(markdown_name: str) -> str:
    notes = LESSON_NOTES[markdown_name]
    outcomes = " ".join(notes["outcomes"])
    return f"""
        <section class="explanation-panel">
          <p class="eyebrow-dark">Context and Intent</p>
          <h2>Before You Run the Example</h2>
          <p>{inline_markup(notes["context"])}</p>
          <h3>What We Are Trying To Achieve</h3>
          <p>{inline_markup(notes["goal"])}</p>
          <h3>Learning Outcomes</h3>
          <p>{inline_markup(outcomes)}</p>
          <h3>Conceptual Layer</h3>
          <p>{inline_markup(CONCEPT_DEEP_DIVE[markdown_name])}</p>
        </section>
"""


def instructor_narrative_panel(markdown_name: str) -> str:
    paragraphs = "\n".join(
        f"<p>{inline_markup(paragraph.strip())}</p>"
        for paragraph in LESSON_NOTES[markdown_name]["narration"].strip().split("\n\n")
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
    previous_href: str | None,
    next_href: str | None,
    current_output: str,
    markdown_name: str,
) -> str:
    nav = (
        f'<nav class="topnav">'
        f'{nav_link("Previous", previous_href)}'
        f'{nav_link("Index", "index.html")}'
        f'{nav_link("Next", next_href)}'
        f"</nav>"
    )
    bottom_nav = nav.replace('class="topnav"', 'class="bottomnav"')
    sidebar = lesson_sidebar(current_output)
    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{html.escape(title)} - Ring0 Training</title>
  <link rel="stylesheet" href="assets/training.css">
</head>
<body>
  <main class="page">
    <header class="brandbar">
      <a class="brand" href="index.html">
        <img src="assets/ring0-logo.svg" alt="Ring0 Training logo">
        <span>
          <span class="brand-name">Ring0 Training</span>
          <span class="brand-subtitle">C++ server examples, phase by phase</span>
        </span>
      </a>
      {nav}
    </header>
    <div class="layout">
      <aside class="sidebar">
        <p class="sidebar-title">Course Modules</p>
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
        {audio_panel(markdown_name)}
        {explanation_panel(markdown_name)}
        {instructor_narrative_panel(markdown_name)}
        {body}
        {bottom_nav}
      </article>
    </div>
  </main>
</body>
</html>
"""


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    ASSET_OUTPUT.mkdir(parents=True, exist_ok=True)
    narration_dir = OUTPUT_DIR / "narration"
    audio_dir = OUTPUT_DIR / "audio"
    narration_dir.mkdir(parents=True, exist_ok=True)
    audio_dir.mkdir(parents=True, exist_ok=True)
    for asset in ASSET_SOURCE.iterdir():
        if asset.is_file():
            shutil.copy2(asset, ASSET_OUTPUT / asset.name)

    for index, (markdown_name, fallback_title) in enumerate(LESSONS):
        source = SOURCE_DIR / markdown_name
        markdown = source.read_text(encoding="utf-8")
        title = page_title(markdown, fallback_title)
        previous_href = output_name(LESSONS[index - 1][0]) if index > 0 else None
        next_href = output_name(LESSONS[index + 1][0]) if index + 1 < len(LESSONS) else None
        body = render_markdown(markdown)
        output = OUTPUT_DIR / output_name(markdown_name)
        narration_path = narration_dir / f"{page_stem(markdown_name)}.txt"
        narration_path.write_text(rich_narration(markdown_name), encoding="utf-8")
        output.write_text(
            wrap_page(title, body, previous_href, next_href, output.name, markdown_name),
            encoding="utf-8",
        )

    print(f"Rendered {len(LESSONS)} training pages into {OUTPUT_DIR}")


if __name__ == "__main__":
    main()
