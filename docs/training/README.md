# Ring0 Application Tutorial

This is a hands-on course, organized by **product capability**, not by
construction history (that course lives in
[docs-process/training/](../../docs-process/training/README.md), one lesson
per build phase, using independent standalone examples).

Here, you build **one application** — an employee directory — from scratch,
and every lesson adds something new to that same, growing codebase. By the
last lesson you'll have touched almost everything Ring0 offers: binding,
persistence, transactions, typed handles, relationships and collections,
snapshots, indexed queries, a network server and client, remote domain
operations, versioned facades, and a graph-shaped org chart.

Source material lives in `docs/training/en`. It is currently a **skeleton**:
each lesson has its structure, learning goals, and pointers to the real
reference documentation already in place, with the step-by-step content and
code still to be written.

Start at [`en/README.md`](en/README.md).

## Relationship to the rest of the docs

- [`docs/DEVELOPER_GUIDE.md`](../DEVELOPER_GUIDE.md) — a single-document,
  faster-paced narrative tour of the same ground. Read that first if you
  want the big picture quickly; come here for a slower, project-based path
  with exercises.
- [`docs/reference/`](../reference/) — precise, per-topic reference
  documentation. Each lesson here links to the reference page(s) it
  exercises.
