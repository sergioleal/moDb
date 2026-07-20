# Ring0 Constitution

This constitution establishes the fundamental and enduring principles of Ring0.
Product, architecture, and implementation decisions must respect them.

## Why Ring0

The name **Ring0** comes from the CPU protection ring where the kernel runs:
the most privileged processor mode—the fastest and most direct execution
environment, but also one where mistakes have immediate consequences.

Ring0 follows the same philosophy. Its core favors performance, explicit
control, and direct access over protective layers. Core users are expected to
understand what they are doing, as they would when writing kernel code. Invalid
use is allowed to fail hard, including crashing the process; the core must not
carry hidden checks or overhead merely to protect callers from their own code.

Safety policies, validation, guardrails, and ergonomic protections belong in
optional **user-space extensions**. They may build safer experiences on top of
Ring0, but must not burden, constrain, or obscure the core.

## Principle I — Built in `C++`, for `C++`

Ring0 is implemented in `C++` and conceived, first and foremost, for `C++`
developers and applications.

This means that:

- the product's core must be written in `C++`;
- the public API must be idiomatic `C++`, with strong types, RAII, and value
  semantics wherever appropriate;
- essential features must not require a virtual machine, managed runtime, or
  foreign-language layer;
- abstractions must preserve performance predictability and allow explicit
  control over memory, concurrency, and object lifetime;
- integrations with other languages may exist, but must not degrade or limit
  the native `C++` experience.

## Principle II — No Inherited Bloat

Ring0 must not accumulate complexity, dependencies, abstractions, or features
merely to conform to guardrails, conventions, or industry standards.

This means that:

- every feature and abstraction must justify its cost through a concrete Ring0
  use case;
- compatibility with established standards is not a goal by itself;
- guardrails must not hide essential behavior, remove deliberate control, or
  impose runtime and maintenance costs disproportionate to their value;
- SQL and similar industry conventions must not shape the product unless they
  directly serve Ring0's native object model;
- convenience and safety layers must remain optional user-space extensions and
  must not burden the core.

Familiarity, market convention, and claims of “best practice” are not
sufficient justification for adding weight to the product.

## Principle III — Performance

Performance is a first-class product requirement, not an afterthought.

This means that:

- latency, throughput, and resource use must be considered at design time;
- abstractions must not hide unpredictable cost or unbounded work;
- the common path must remain efficient; optional features must not tax
  workloads that do not use them;
- performance claims must be measurable and regression-sensitive;
- when trade-offs are necessary, the cost must be explicit and justified.

Ring0 prefers predictable, inspectable performance over convenience that
obscures what the system does with CPU, memory, and I/O.

## Principle IV — Crash Recoverability

After an abrupt process or host failure, Ring0 must return to a consistent and
usable state without silent data corruption.

This means that:

- durability mechanisms must make committed work recoverable;
- recovery must restore a coherent snapshot of committed state;
- incomplete or torn writes must not leave the database undefined;
- recovery behavior must be deterministic, testable, and observable;
- operators must be able to verify whether recovery succeeded.

Failing hard because of invalid use does not excuse storage corruption. Process
safety and durable-state recoverability are separate concerns.

## Principle V — Disaster Recovery

Ring0 must support recovery from loss of the primary machine, storage volume,
or data center. Its primary disaster-recovery mechanism must be continuous,
real-time streaming of durable commits to external replicas.

This means that:

- committed state must stream continuously to replicas in external failure
  domains;
- replicas must preserve commit order and apply only complete, durable state;
- replication lag and stream health must be measurable and observable;
- recovery must provide a defined, testable path for promoting or restoring an
  external replica;
- backups may complement streaming replication, but must not replace it as the
  primary disaster-recovery mechanism;
- gaps, divergence, and incomplete copies must fail loudly rather than produce
  a silently incorrect database.

Local crash recovery and disaster recovery are distinct: surviving a process
crash is not the same as surviving loss of the primary store.

## Principle VI — Streaming with Backpressure

Communication across Ring0 boundaries must be asynchronous streaming with
backpressure. Producers and consumers must make incremental progress instead
of blocking while waiting for complete blocks, batches, or result sets.

This means that:

- data must flow as soon as it becomes available;
- consumers must control demand so producers cannot overwhelm downstream
  memory or processing capacity;
- producers must not materialize an entire response before sending its first
  result;
- consumers must not wait for an arbitrary block or batch to fill before they
  can make progress;
- streaming and backpressure must propagate end to end, from storage and
  server-side execution through the network to the client;
- bounded buffering may absorb scheduling and transport variance, but must not
  turn the pipeline into hidden batch processing.

Waiting for I/O readiness is an implementation detail; blocking a workflow
until a whole data block is produced is not an acceptable communication model.

## Principle VII — Domain Code on the Server

Ring0 executes domain logic where the data lives: inside the server process.
The goal is to minimize round-trips and bulk data transfer between client and
database.

This means that:

- domain operations must run as server-side units of work against the live
  object store;
- clients should send intent and receive results, not pull large object graphs
  solely to decide and write back;
- domain code must stay close to identity, transactions, and the catalog
  without exposing physical pages or the WAL;
- the design must favor shipping code and small inputs over shipping data.

## Principle VIII — Bare Metal

Ring0 is designed and optimized for direct execution on bare-metal machines.
Containers and serverless platforms are not product targets and must not shape
its architecture, runtime, packaging, or operational model.

This means that:

- the primary deployment model must be a native process running directly on
  the host operating system;
- the design must favor explicit control of CPU, memory, storage, networking,
  scheduling, and I/O;
- containers, orchestration platforms, functions as a service, and other
  serverless environments must not be required or treated as supported
  deployment targets;
- architecture decisions must not sacrifice bare-metal performance,
  predictability, or hardware control to accommodate container or serverless
  constraints;
- optional third-party packaging may exist outside the core, but must not
  influence or burden the product.

## Principle IX — Graphs

Relationships between objects with identity are a first-class graph: edges are
native to the model, not an afterthought layered on tables or join queries.

This means that:

- associations and ownership among identified objects form a traversable graph
  of vertices and edges;
- the product must support discovering, walking, and reasoning over that graph
  under consistent snapshots;
- graph operations belong next to the object model and domain execution, not
  in a separate product or SQL-shaped substitute;
- edge direction and association-versus-composition semantics must remain
  explicit;
- graph algorithms and views must respect identity, integrity, and recovery.

## Governance

- New principles are added through deliberate, documented amendments.
- Changes must explain their motivation and consequences for the product.
- In case of conflict, this constitution prevails over prior plans, protocols,
  and architectural decisions.

---

**Product:** Ring0  
**Constitution version:** 0.9.0  
**Ratified principles:** 9
