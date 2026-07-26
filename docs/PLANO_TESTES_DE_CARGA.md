# Ring0 Load Test Plan

- Status: all subphases (A through R) implemented, plus a post-implementation
  review pass; see [docs-process/PLANO_IMPLEMENTACAO_CARGA.md](../docs-process/PLANO_IMPLEMENTACAO_CARGA.md)
  for the up-to-date tracker (20/20 subphases done)
- Version: 1
- Date: 2026-07-25
- Scope: volume and duration load over the object model, with local execution
  (embedded and loopback) and remote execution, selectable by subset, with a
  versioned historical series

## Quick Start

**Run a load test:**

```powershell
.\scripts\run-load.ps1 -ConfigPath loadtests\config\load-smoke.yaml
```

```bash
./scripts/run-load.sh --config loadtests/config/load-smoke.yaml
```

Or call `modb_load` directly:

```powershell
.\build\debug\modb_load.exe run --profile load-smoke --environment desktop-windows
```

`--environment` must be set to a registered id (`loadtests/environments.json`,
§4.4) — otherwise the result is not indexed into history (rejected for
missing provenance, §13.3).

**View the results:**

```powershell
Invoke-Item .\loadtests\dashboard\index.html
```

Drag `load-history/series.jsonl` onto the page (or click "Open
series.jsonl…"). See §13.11 for details — the dashboard only reads rollup
lines (`"schema":"modb.loadtest.rollup"`); it never accepts the raw
per-campaign file directly.

**Other useful commands:**

```text
modb_load list-profiles                              # list available profiles
modb_load list-cases --profile NAME                  # preview a profile's cases without running them
modb_load run --profile NAME --dry-run               # print the estimated plan (disk/time) without running
modb_load trend --case ID --metric ops_per_second    # print the history for one case/metric
modb_load gate --case ID --metric ops_per_second     # pass/fail against history (CI-friendly exit code)
modb_load resume <file.partial>                      # resume an interrupted campaign
```

Everything below this point is the detailed design and rationale (dimensions,
workloads, budget, history schema, dashboard internals, etc.) — read on only
if you need the "why", not just the "how".

## 1. Objective

Measure Ring0's behavior as the **user volume** grows from 10 thousand to 1
million objects, under mutation patterns of increasing complexity, both
embedded and through the server (local and remote).

A load test answers different questions than
[PLANO_BENCHMARKS.md](PLANO_BENCHMARKS.md):

| | Benchmark (Phase 10) | Load test (this plan) |
|---|---|---|
| Question | what is the latency/throughput of one operation | does the system stay correct, stable and predictable at scale |
| Duration | short repeated samples (≥ 250 ms) | one long pass per case (minutes to hours) |
| Unit | operation | volume campaign (N users start to finish) |
| Typical failure | 10% regression | file growth, fragmentation, per-window degradation, disk/memory exhaustion, corruption |
| Horizon | one comparison against a baseline | continuous historical series, with detectable slow drift (§13) |

The two plans deliberately share infrastructure: JSONL writer, environment
collection, seeds, `sha256`, and the comparator. What changes is the axis of
variation and the acceptance criteria.

## 2. Terminology and explicit premise

- **user**: a persistent `User`-type object (record), not a human session.
  The 10k-to-1M scales in this plan are **object counts**.
- **session**: a concurrent client connection. Concurrency exists as a
  secondary dimension (§4.5) with modest values, because the engine has a
  **single transaction writer** (Phase 5) and the server assumes few
  connections per instance
  ([ADR-011](decisions/ADR-011-concorrencia-do-servidor.md)). Scaling
  sessions does not scale writes; it measures contention and fairness.

If the original intent was load from *concurrent sessions* rather than
record volume, dimension D1 and the concurrency dimension swap roles — the
rest of the plan (workloads, targets, subset selection, result format)
remains valid unchanged.

## 3. Principles

1. No campaign completes the full cartesian matrix; the matrix exists to be
   filtered (§6).
2. Every case declares estimated disk, time and memory upfront; exceeding
   the budget produces a recorded `skipped_budget`, never silent truncation.
3. Load does not replace correctness: each phase validates invariants
   outside the measured region, and the campaign fails if validation fails,
   regardless of throughput.
4. Per-window results, not just aggregates: temporal degradation only shows
   up in series.
5. Deterministic data, recorded seed, versioned dataset.
6. An interrupted case leaves a readable, resumable `.partial` file.
7. Write and read, creation and removal, measured in separate, named phases.
8. The same binary runs embedded, loopback and remote; the target is a
   parameter, not a code fork.
9. Every run deposits a point in the historical series (§13). A measurement
   that cannot be compared with one from three months ago was wasted work.
10. The historical series is append-only and versioned; raw results are
    disposable, historical points are not.

## 4. Dimensions

### 4.1 D1 — Scale (user count)

| id | objects | use |
|---|---|---|
| `1k` | 1,000 | development smoke test; outside the official load range |
| `10k` | 10,000 | floor of the official range; runs on any machine |
| `100k` | 100,000 | reference case for historical comparison |
| `250k` | 250,000 | intermediate point to check linearity |
| `500k` | 500,000 | cache/buffer pool pressure |
| `1M` | 1,000,000 | ceiling of the range; requires a declared disk/time budget |

The 10k → 100k → 1M progression is deliberately multiplicative: it lets us
check whether time, bytes per object and pages per object grow linearly or
super-linearly. `250k` and `500k` exist to locate the knee when 1M degrades.

### 4.2 D2 — Workload (increasing complexity)

A fixed ladder. Each rung contains the previous rung's operations plus one
new source of stress. Stable ids, never reused with a different meaning.

| id | phases | what it stresses | final invariant |
|---|---|---|---|
| `create_only` | create | pure ingestion, file/WAL growth, per-batch commit cost | count == N, logical hash of the set matches |
| `create_delete_forward` | create → delete (creation order, FIFO) | removal with perfect locality, space reclaimed in order | count == 0, no resolvable id |
| `create_delete_reverse` | create → delete (reverse order, LIFO) | last-page compaction, shrink path | count == 0, final size ≤ `forward`'s final size |
| `create_delete_interleaved` | create → delete by seeded stride/Zipf | real fragmentation, free list, partial reuse | count == 0, fragmentation recorded |
| `crud_full` | create → read → update in-place → grow update → shrink update → delete | full cycle, record rewrite, movement between pages | read values == expected per object; count == 0 |

Phase rule: every phase is timed, validated and summarized separately. A
`crud_full` at 1M produces six `phase_summary` records, not a single number.

#### 4.2.1 Additional workloads — isolating specific engine behaviors

The ladder above measures the storage path (heap, pages, fragmentation). It
says nothing about cache, MVCC, blobs, referential integrity, recovery or
replication — each of these has its own failure mode that only shows up
under real volume and real duration, not in a few-second benchmark sample.
The workloads below isolate one behavior at a time, the same way the ladder
isolates one mutation pattern at a time. None of them replace the ladder;
"all workloads" in the profile descriptions (§6.2) refers only to the basic
ladder — these require explicit selection via `--workload` or the
`load-behavior` profile.

| id | phases | what it stresses | final invariant |
|---|---|---|---|
| `read_hotspot` | create → read (Zipf over a fixed working set) | buffer pool/page cache pressure under skewed reads | read values == expected; cache hit rate recorded |
| `range_scan_sweep` | create → scan (selectivity 0.01%–100%) | index vs. full-scan cost as selectivity and volume vary | returned count == expected per selectivity; plan (index/scan) recorded |
| `mixed_oltp` | single phase, concurrent sessions emitting create/read/update/delete at a configured ratio (default 5/80/10/5) | real contention and tail latency under a mix — not under one isolated, repeated operation | final count reconciles (created − removed); deterministic sample checksum matches |
| `snapshot_hold` | create → open snapshot(s) → churn (create/update/delete) → close snapshot(s) | MVCC version retention under real volume and duration, GC on close | reading through the open snapshot stays identical to the state at open throughout the churn; retained versions and bytes recorded |
| `blob_lifecycle` | create (with blob) → read/stream → grow → shrink → delete | `BlobStore` under varying volume and sizes (1, 16, 256 MiB) | byte-for-byte hash of the read blob == written; space reclaimed after delete |
| `cascade_delete` | create_hierarchy (depth × width) → cascade_delete (root) | referential integrity and cascade-removal cost scaling with descendant count | zero orphan refs; total removed == total created |
| `oversubscribed_churn` | create → interleaved churn, cache explicitly smaller than the working set | graceful vs. catastrophic degradation once volume exceeds the cache — a real-volume version of the `storage.buffer_pool.oversubscribed` scenario (Phase 10) | same invariants as `create_delete_interleaved`, plus recorded eviction/reread ratio |
| `restart_recovery` | churn → kill at a defined point (mid-transaction, post-commit, mid-checkpoint) → restart → verify | WAL replay cost and correctness scaling with volume | post-recovery logical hash == last durable commit's hash; recovery time recorded |

`range_scan_sweep` and `cascade_delete` formalize, respectively, the old
`crud_query` and `crud_relationships` placeholders — they stop being
placeholders and get defined phases and invariants.

**Implemented in Subphase L**: `read_hotspot` and `range_scan_sweep`,
`embedded` only. `read_hotspot` uses
`database.page_file().buffer_pool().metrics()` for the real hit rate
(`PhaseMetrics.cache_hit_rate`, a new field — `-1.0` on phases that don't
measure it) and a Zipf sampler with a precomputed CDF over the working set;
it validates read values by comparing the hash in the same order they were
read (not creation order). `range_scan_sweep` creates an index on `User.id`
and runs one phase per selectivity (0.01%/0.1%/1%/10%/100%), each phase
named with the real `AccessMethod` from `query::QueryPlan`
(`scan_1pct_index_scan`, for example) — the "recorded plan" acceptance
criterion becomes part of the phase name itself, not a new schema field.

**Implemented in Subphase M**: `mixed_oltp`, the only workload that actually
reads `c.concurrency` — this closes debt D1 for that dimension
(`unimplemented_dimension_reason` now only rejects concurrency≠1 for other
workloads). `params.concurrency` sessions (real `std::thread`s) emit
create/read/update/delete (5/80/10/5) against the SAME `Database`, each
whole operation (begin+engine+commit+bookkeeping) under a single
`std::mutex` — the engine is single-threaded (ADR-011), so this is real
contention on the entry queue, not real parallelism inside the engine (the
same design `Server::engine_mutex_` already uses for network sessions).
Reconciliation genuinely counts via `query<User>().stream()` (not just
trusting in-memory bookkeeping), and the sample checksum orders surviving
ids by a fixed stride, the same discipline as `crud_full`'s
`sample_stride`. `write_amplification`/`space_amplification` stay at `0.0`
(not computable under concurrent interleaved create/update/delete, the same
"don't invent it" convention already used in `create_delete_embedded`).
Verified live with `--case load.mixed_oltp.embedded.10k` (60,000
operations, 0 errors, `hash_match:true`) and with 4 real threads in a
smaller test.

**Implemented in Subphase N**: `snapshot_hold`. Opens a real `Snapshot`
(`database.snapshot()`), rereads the whole working set through it before
the churn, does update/delete/create on the LIVE state (never touching the
same object twice — a second change while the same snapshot is still open
fails with `snapshot_conflict`, since there is only room for one `previous`
version at a time), rereads through the SAME still-open snapshot again
(has to match the pre-churn read byte for byte), and only then closes the
snapshot (`std::optional<Snapshot>::reset()`) and calls
`collect_garbage()`. After closing, a NORMAL read confirms the state truly
reflects the churn (removed objects don't resolve; updated ones show the
new value). `PhaseMetrics` gains `retained_versions` (a new field,
`data_record_count()` minus the current live set — extra versions the
engine was forced to retain by the open snapshot). Verified live at 10k
objects: `retained_versions=7666` during the hold, `hash_match:true`,
`all_deleted:true`.

**Implemented in Subphase O**: `blob_lifecycle` over a real `BlobStore` —
not `dataset_user_blob` as a formal dataset module (§7), but an ad hoc
deterministic generator (`deterministic_blob_pattern`, a PRNG seeded by
`seed`) directly in the workload, simpler and sufficient for what this
subphase needs to validate. Sizes reduced from 1/16/256 MiB (§4.2.1) to 64
KiB/1 MiB/16 MiB — 256 MiB per case would make the verification routine
minutes slower without exercising any additional code (the BLBP page chain
is already thoroughly exercised from a few hundred KiB). Five phases
(create/read/update_grow/update_shrink/delete), each verifying content byte
for byte (both `read()` AND streaming `read_chunks()`, both against the
same buffer). **A genuine finding, not worked around**: `BlobStore` has no
free list (per the source code's own comment, `blob_store.hpp`) — removed
pages stay orphaned in the file, so "space reclaimed after delete" (the
§4.2.1 invariant) is not satisfied by this blob MVP; `reclaimed_bytes`
reports the real difference (typically 0, never invented as positive)
instead of faking a reclamation the engine doesn't do. Verified live with
`--case load.blob_lifecycle.embedded.1k`: `hash_match:true`,
`all_deleted:true`, `reclaimed=0` (honest).

**Implemented in Subphase P**: `cascade_delete`. An N-ary tree encoded as
"first child / next sibling" — only 2 `OwnedRef` fields (not one per
child), so any width works: removing a node cascades into `first_child`,
which cascades into its `next_sibling`, which cascades into its own,
covering the whole subtree and all siblings with a single
`database.remove(tx, root)` call — the graph walk is done by the ENGINE
itself (`Database::remove_cascade`), not by this workload's code. Fixed
depth of 4, width derived from `object_count`
(width≈object_count^(1/4)). Verified live with `--case
load.cascade_delete.embedded.1k`: a tree of 1555 nodes (width 6, depth 4:
6⁴+6³+6²+6+1), cascade-removed with `still_resolving=0`.

**Post-implementation review**: tree creation (`create_hierarchy`) now
respects `batch` (§4.5) — it commits periodically during the recursion
instead of holding the whole tree (up to ~1.08M nodes at
`object_count=1M`, width 32) in a single transaction. The cut point is
always right after a node is created (never in the middle of a node with
pending children), because references only ever point backward — safe at
any depth. **Cascade removal of the root is still ONE
`database.remove(tx, root)` call, in a single transaction**:
`Database::remove_cascade` is an atomic engine operation with no way to
paginate from the outside (out of scope for this load harness — it would
require a new engine API, not just something in `modb_load`).

**Implemented in Subphase Q**: `oversubscribed_churn`. Same logic as
`create_delete_interleaved` (same phases, same `reorder_for_delete`), but
`Database::create` receives an explicit `cache_capacity` (a parameter that
already existed in the API, just unused by any workload until now) — 10%
of the estimated page count for the working set, forcing real eviction
instead of relying on a cache that's "almost enough". The delete phase's
`cache_hit_rate` (buffer pool metrics reset right after create, so the
reading isn't diluted by the initial fill) is the "eviction/reread ratio"
from the acceptance criterion. The `load-behavior` profile (§6.2) already
existed in the catalog with the 7 additional workloads — it resolved, but
6 of them had no dispatch until this wave of subphases (L-Q); now it
resolves and dispatches end to end (`--dry-run` lists the 7 real cases). A
genuine finding: even with the cache at ~10% of the estimate, the hit rate
measured at 10k objects came out at ~93.6% — the stride-delete locality
(§4.2, Subphase D) apparently favors the reduced cache well; more severe
degradation should show up at larger scales or with an even smaller cache,
not investigated further in this subphase.

**Implemented in Subphase R (with one deliberate simplification)**:
`restart_recovery`. Normal churn (full commit) followed by ONE
deliberately interrupted commit via
`Transaction::commit(CommitPhase::stop_after_commit_record)` — durable in
the WAL, data pages not yet applied, the same test seam
`tests/recovery_test.cpp` already uses — after that the in-memory
`Database` is closed (goes out of scope, detached) and REOPENED from the
same file (`Database::open`), triggering a real WAL replay.
**Simplification**: the "crash" is simulated via a failpoint inside the
SAME process, not a `kill -9`/`TerminateProcess` of a separate process —
there was no real kill/restart harness anywhere in the repository (not even
for the existing recovery test suites), and building one from scratch
(process spawning, crash-point synchronization, cross-platform Windows/
Linux) is a project of its own, not an afternoon's work. What DOES get
proven is exactly the §4.2.1 acceptance criterion ("post-recovery logical
hash == last durable commit's hash") — the engine's actual WAL replay path,
not an in-memory simulation that never touches the file. The real kill
harness (Windows and Linux) remains future work (§17, risk 13). Verified
live with `--case load.restart_recovery.embedded.1k`: `hash_match:true`,
recovery phase measured at ~34ms for 1000 objects.

Two additional workloads depend on infrastructure the generic harness
(`target.hpp`, §14) doesn't cover yet; they stay **out of every profile**
until that infrastructure exists — the same treatment given today to
`primary_storage=wal_only` (§4.5 secondary dimensions, risk 2 in §17):

| id | phases | what it stresses | depends on |
|---|---|---|---|
| `schema_evolution` | create_v1 → concurrent read/write under v1 and v2 bindings → verify | migration/compatibility cost without stopping the world | a harness for two simultaneous `Binding` versions — doesn't exist in the generic `target.hpp` |
| `replica_catchup` | primary_churn → measure lag → burst → catchup | replication lag growing with volume; recovery time after a burst | a read replica orchestrated by `modb_load` itself — today `primary_storage` is just a primary-side parameter, not a topology with a follower |

### 4.3 D3 — Execution target

| id | topology | measures | doesn't measure |
|---|---|---|---|
| `embedded` | in-process, no network | pure engine: storage, WAL, object model | protocol, network serialization |
| `loopback` | server + TCP client on `127.0.0.1` | protocol, frames, backpressure, session cost | real latency and bandwidth |
| `remote_colocated` | load binary runs on the remote host, client and server on the same host | engine and protocol on the server's hardware/FS | WAN network |
| `remote_client_local` | remote server, client on the local machine | real network: RTT, bandwidth, jitter, TTFR | engine isolation |

`remote_colocated` is the default target for scale numbers (the network
doesn't pollute the measurement). `remote_client_local` is the target for
network questions and is limited to the `10k`/`100k` scales, because 1M
objects crossing a WAN measures the link, not the database.

**Implemented in Subphase G (minimal version).** `loopback` only works for
`create_only` (`loadtests/target_client.cpp` +
`loadtests/loadtest_facade.cpp`): a real `net::Server` comes up on
`127.0.0.1` (OS-assigned port), an `app::ServerConnection` connects and
invokes `CreateBatch` (a facade `ops::Operation`, one batch per `--batch`,
the same per-batch-commit semantics as `embedded`), and hash validation
rereads EVERYTHING via `collect()` (a remote query), sorting by the logical
`id` field before comparing — a remote scan's order is not guaranteed to be
creation order, unlike `embedded`, which rereads by the ids themselves in
creation order. `create_delete_*`/`crud_full` still reject `loopback` in
their own wrapper (`workloads/*.cpp`), not implemented in this subphase.
Actual network metrics (bytes/frames/syscalls/TTFR, the "measures" column
above) are **not** collected yet — client and server run in the SAME
process (a `std::thread` accepts the connection), so `peak_rss_bytes`
reflects both combined, not an isolated network cost; `latency_ns` has
per-BATCH granularity (one network round trip per `invoke`), not per
object like `embedded` — the two numbers aren't point-for-point comparable
across targets. Closing these gaps (separate processes, real network
metrics) is left for a future iteration of this subphase.

**Post-implementation review.** `target_client.cpp` gained a safety net
(RAII) for joining the acceptor thread — an exception escaping the client/
server block no longer leaves the thread joinable for `~std::thread` to
call `std::terminate()` on.

`Server::request_stop()` used to close the listener via `close(fd)` against
a thread blocked in `accept()` — this reliably unblocked it on Windows, but
was unspecified behavior on POSIX (on Linux, `accept()` tends to stay
blocked, hanging `acceptor.join()` on the `connect()`-failure path). Fixed
in `src/net/native_socket.cpp`/`native_socket.hpp` (not in
`server.cpp`/`server.hpp`, which were under concurrent edit by another
process in this same tree — the "self-pipe" trick stayed entirely
contained inside the native socket): the listening socket gets its own
pipe created in `listen()`; `accept()` first waits in `poll()` for either
the socket or the pipe, never blocking inside a real `::accept()` without
knowing a connection is ready; `close()` writes to the pipe to wake up
whoever is waiting, instead of relying on closing the listening fd itself
(which is the unspecified behavior). **Still not verified on real Linux**
— this session's development environment is Windows (where the file's
POSIX branch doesn't even compile), and SSH access to `linux-remoto` was
never tested (§6.4/scripts/run-remote-load.ps1). The full suite (including
`modb.native_socket`/`modb.operation_server`/`modb.app_server_connection`)
passes on Windows with no regression, but that only exercises the
`#ifdef _WIN32` branch, which wasn't changed.

### 4.4 D4 — Registered environment

D3 answers "which topology" (what runs where, relative to whom); D4 answers
"on which registered machine" — the two are orthogonal. Local topologies
(`embedded`, `loopback`) run on a single registered environment (the one
executing the command); remote topologies (`remote_colocated`,
`remote_client_local`) name roles (client/server) that each resolve to a
registered environment, possibly two different ones.

Without an identified environment, "bench machine" is a string typed from
memory on every run — exactly how risk 9 (§17) happens. A named registry
exists to be picked from a list, not retyped.

Catalog: `loadtests/environments.json`, versioned in Git — no secrets, just
identity and how to reach the machine (the same practice already used in
`scripts/run-remote-benchmark.ps1`, which today has the IP hardcoded in the
script instead of registered; that moves out as part of this dimension,
not just in Subphase H).

```json
{
  "schema": "modb.loadtest.environments",
  "schema_version": 1,
  "environments": [
    {
      "id": "desktop-windows",
      "label": "My desktop (Windows, dev)",
      "kind": "local",
      "host_class": "dev-windows",
      "os_hint": "windows",
      "notes": "Development machine; noisy, don't use for gating."
    },
    {
      "id": "linux-remoto",
      "label": "Remote Linux server (bench)",
      "kind": "ssh",
      "host_class": "bench-linux-01",
      "os_hint": "linux",
      "connection": {
        "host": "161.35.9.43",
        "default_user": "root",
        "remote_work_dir": "/tmp/modb_load",
        "binary_name": "modb_load"
      }
    }
  ]
}
```

Fields:

- `id`: stable slug used by `--environment`; never renamed — renaming means
  registering a new id and marking the old one `"deprecated": true`, so as
  not to invalidate old series that reference it;
- `kind`: `local` (process on the host where the command runs) or `ssh`
  (remote host over OpenSSH — credentials never in the file, requested
  interactively, same as the current script);
- `host_class`: the comparability label from §13.4, resolved from the
  registry instead of typed on every run — closes risk 9;
- `connection`: only for `kind=ssh`; host, default user, remote directory
  and binary name; no password or token;
- `notes`: free text, goes into `run_note` when relevant (e.g. "don't use
  for gating").

CLI: `--environment ID[,ID...]` selects where the load command actually
runs. Implemented today, ahead of Subphase A's implementation order, in
`scripts/run-remote-benchmark.ps1 -Environment <id>`: the script resolves
host, user and remote path from the catalog and rejects any `kind` other
than `ssh`. `modb_load`'s Subphase A adopts the same interface.

`environment` does not enter the `case_id` (keeps principle 3 of stable
ids), but is recorded in `case_start`, in the rollup (`environment`, §13.3)
and is a first-class filter in the CLI and dashboard (§13.11).

### 4.5 Secondary dimensions

Fixed at a default value; varied only for cases targeting a specific risk.

| dimension | values | default |
|---|---|---|
| user payload | `slim` (~64 B), `normal` (~256 B), `fat` (~4 KiB) | `normal` |
| objects per commit | 1, 100, 1,000, 10,000 | 1,000 |
| concurrent sessions | 1, 4, 16 | 1 |
| concurrent readers during write | 0, 2, 8 | 0 |
| durability | `sync_real`, `disabled_diagnostic` | `sync_real` |
| cache | `warm`, `database_reopen`, `oversubscribed` | `warm` |
| `primary_storage` | `full`, `wal_only` | `full` |

`durability=disabled_diagnostic` is never published as a durable load
number; it exists to isolate CPU/codec cost from `fsync` cost.

`primary_storage=wal_only`
([ADR-017](decisions/ADR-017-primary-wal-only-sem-arquivos-de-dados.md))
changes the nature of the test: on the primary there is only WAL, so "file
growth" becomes log growth and the relevant question is retention/
checkpointing. `wal_only` cases only appear in the heavy profiles, and
always with the follower measured alongside.

## 5. Case identity

```text
load.<workload>.<target>.<scale>[.<variant>]
```

Examples:

```text
load.create_only.embedded.100k
load.create_delete_reverse.loopback.1M
load.crud_full.remote_colocated.100k.c16
load.create_only.embedded.1M.payload_fat
```

The variant suffix appears **only** when some secondary dimension deviates
from the default, and uses the dimension's short name (`c16`, `payload_fat`,
`batch1`, `nosync`, `reopen`, `walonly`). A case with all defaults never
carries a suffix — this keeps historical ids stable as new secondary
dimensions are added.

The result record always carries **every** effective parameter, even the
ones that don't appear in the id.

## 6. Subset selection

Core requirement: any slice of the matrix must be runnable in isolation,
without editing code.

### 6.1 Command-line interface

```text
modb_load list-cases [selectors]
modb_load run [selectors] [budget] [--output-dir DIR] [--work-dir DIR] [--seed N]
modb_load resume <file.partial>
modb_load list-profiles
```

Selectors, all combinable, all accepting a comma-separated list or a
repeated flag:

| flag | effect |
|---|---|
| `--profile NAME` | starting point: a predefined set of cases (§6.2) |
| `--scale 10k,1M` | restricts D1 |
| `--workload create_only,crud_full` | restricts D2 |
| `--target embedded,loopback` | restricts D3 |
| `--environment ID` | restricts D4 — resolves host/kind from the `loadtests/environments.json` catalog |
| `--case ID` | exact case; ignores the profile and other selectors |
| `--filter SUBSTR` | substring match against `case_id` (same semantics as `modb_bench --filter`) |
| `--exclude SUBSTR` | removes from the set, applied after all filters |
| `--concurrency 1,16` | session-count variant |
| `--payload normal,fat` | payload variant |
| `--repeat N` | repeats the whole case (default 1; ≥ 3 for A/B decisions) |

Composition semantics, unambiguous:

1. the profile defines the initial set;
2. each dimension selector **intersects** with that set;
3. `--case` replaces everything with an explicit list;
4. `--exclude` subtracts last;
5. an empty set is an error with exit code 2 and a message listing what was
   left at each step — never "ran zero cases successfully".

### 6.2 Profiles

| profile | set | target duration |
|---|---|---|
| `load-smoke` | `1k` × all workloads × `embedded` | 1–3 min |
| `load-local` | `10k`,`100k` × all workloads × `embedded`,`loopback` | 20–40 min |
| `load-standard` | `100k` × all workloads × `embedded`,`loopback`,`remote_colocated` + `1M` × `create_only`,`crud_full` × `embedded` | 2–4 h |
| `load-heavy` | `250k`,`500k`,`1M` × all workloads × `embedded`,`remote_colocated` + pairwise secondary variants | 12–24 h |
| `load-remote` | `10k`,`100k` × `create_only`,`crud_full` × `remote_colocated`,`remote_client_local` | 30–60 min |
| `load-soak` | `500k` × `create_delete_interleaved` looped for a fixed duration | 1–24 h |
| `load-behavior` | `100k` × `read_hotspot`,`range_scan_sweep`,`mixed_oltp`,`snapshot_hold`,`blob_lifecycle`,`cascade_delete`,`oversubscribed_churn` × `embedded` | 1–2 h |
| `load-diagnostic` | empty; requires explicit selectors | no target |

`load-heavy` is pairwise on secondary dimensions, not cartesian: each
non-default value appears at least once, without multiplying the matrix.

**Implemented in Subphase K.** `load_heavy_cases()` adds, on top of the
primary product (ladder × target × scale): 2 cases with `payload=fat`
(`create_only` and `crud_full` at `250k`) and 2 `mixed_oltp` cases with
`concurrency=4`/`16` (the only workload with real concurrency dispatch,
Subphase M). It does **not** include `durability`/`cache`/
`primary_storage`/`readers` — no workload has dispatch for non-default
values of these dimensions yet (§4.5), so promising a value there would be
exactly the D1 debt `unimplemented_dimension_reason` exists to reject.
`load-soak` remains a single real case (`create_delete_interleaved` at
`500k`) — "looped for a fixed duration" (running until N hours have
elapsed, not a fixed repeat count) has no dedicated mechanism yet;
`modb_load run --profile load-soak --repeat N` (Subphase A) is today's
available approximation. "All workloads" in
`load-smoke`/`load-local`/`load-standard`/`load-heavy` refers only to the
basic ladder (§4.2); the §4.2.1 workloads only enter via `load-behavior` or
explicit selection. `restart_recovery` stays out of every automatic
profile — it deliberately kills the process, so it only runs under
explicit `--workload restart_recovery`, with the operator aware.
`schema_evolution` and `replica_catchup` stay out of every profile until
the infrastructure they depend on exists (table in §4.2.1).

### 6.3 Planning before running

`list-cases` and `run --dry-run` print, to stderr, the final list with a
per-case and total estimate:

```text
load.create_only.embedded.100k        objects=100000  disk~=?  time~=?
load.crud_full.embedded.1M            objects=1000000 disk~=?  time~=?
--
14 cases  estimated peak disk ~= ?  estimated total time ~= ?
```

Estimates come from the calibration table (§10), stored in the repository
and updated by measurement, never by guessing. Until calibration exists,
the command prints `?` and `run` requires `--accept-unknown-budget`.

### 6.4 Resume

Every completed case is one `case_summary` line in the JSONL. `resume`
reads the `.partial`, reconstructs the set of already-completed cases, and
runs only the rest, appending to the same file. This makes the
`load-heavy` matrix viable across discontinuous maintenance windows.

**Implemented in Subphase F.** `resume <file.partial>` reconstructs every
pending case from its own already-recorded `case_start` (field names, not
the `case_id` text — which doesn't decode secondary dimensions outside the
default), treats every `case_id` with a `case_summary` **or** a
`case_error` as "done", and refuses to resume (an explicit error, not a
silent rerun) a case that was interrupted before emitting its own
`case_start`. `--work-dir` is optional (not persisted in the schema, §12;
by default it uses the `.partial`'s own directory, same as `run` without
`--work-dir`).

### 6.5 YAML configuration (`scripts/run-load.ps1` / `run-load.sh`)

**Implemented.** The two scripts are a local-execution wrapper over the
§6.1 selectors — not a format of `modb_load` itself. They read
`loadtests/config/*.yaml`, a restricted YAML subset documented in the
header of `loadtests/config/load-local.yaml` (key: scalar value; key:
followed by `  - item` for a list; no quotes, no single-line list, no
nesting beyond one level — hand-parsed, a format error is a parse failure,
not best-effort).

```bash
./scripts/run-load.sh                                  # uses load-local.yaml
./scripts/run-load.sh --config loadtests/config/x.yaml --dry-run
./scripts/run-load.sh --environment linux-remoto        # overrides the yaml
```

```powershell
.\scripts\run-load.ps1
.\scripts\run-load.ps1 -ConfigPath loadtests\config\x.yaml -DryRun
.\scripts\run-load.ps1 -Environment linux-remoto
```

Every YAML key maps to a §6.1 flag (`scale`/`workload`/`target`/
`environment`/`concurrency`/`payload`/`case` are lists, becoming
`--flag value1,value2`); `accept_unknown_budget` and `dry_run` are
booleans — the second one passes `--dry-run` to `modb_load` itself (it
just prints the plan, §6.3), distinct from the script's own
`-DryRun`/`--dry-run` (which doesn't even require the binary to exist, it
just shows the resolved command).

The scripts validate `environment:` ids against
`loadtests/environments.json` (§4.4) **before** building the command — a
typo fails there, not after `modb_load` has already started — and warn
(don't block) when the environment is `kind=ssh`, because these two
scripts run locally; remote dispatch is still
`scripts/run-remote-benchmark.ps1` (or the future `run-remote-load`).
Validation in `run-load.sh` is best-effort: without `jq` installed, it
warns and proceeds, because `jq` is not a required project dependency.

After running (`--environment` must be set — in the YAML or via
`-Environment`/`--environment`, otherwise the point is rejected during
indexing for missing provenance, §4.4), the aggregated result goes into
`load-history/series.jsonl`. To view it, see §13.11 (Dashboard).

## 7. Dataset

`user_v1`, synthetic and deterministic:

| field | type | content |
|---|---|---|
| `id` | u64 | sequential starting at 1 |
| `login` | string ≤ 16 | derived from the id, unique |
| `email` | string ≤ 32 | derived from the id, unique |
| `display_name` | string ≤ 24 | versioned synthetic corpus |
| `created_at` | i64 | fixed base + id × step (never the real clock) |
| `status` | i32 | enum with a declared distribution |
| `filler` | bytes | sizes the payload for `slim`/`normal`/`fat` |

Rules: generation happens outside the measured region; `dataset_id`,
`dataset_version`, `seed`, `generator_commit` and `logical_hash` are
recorded; no real data, no wall-clock time inside the content (would break
hash reproducibility).

`create_delete_interleaved`'s removal order comes from a generator with a
recorded seed, not the global `rand()`.

`blob_lifecycle` (§4.2.1) uses `dataset_user_blob`, a separate variant —
not an extension of `user_v1` — that adds a blob field of configurable size
(1, 16, 256 MiB). The other workloads keep using `user_v1` with no blob;
risk 15 (§17) explains why.

## 8. Metrics

Per phase (`create`, `read`, `update_inplace`, `update_grow`,
`update_shrink`, `delete`) and per progress window:

**Time and rate** — phase duration, objects/s, commits/s, per-operation
latency (p50, p95, p99, p99.9, max) measured on the emitting side, full
histogram preserved, TTFR on read phases.

**Resources** — user/system CPU, current and peak RSS, allocations and
peak allocation, physical and logical reads/writes, `fsync` and its
latency, pages read/written/allocated/reused/evicted, cache hit rate.

**Space and amplification** — database and WAL size before and after each
phase, bytes persisted per logical object, write/space amplification,
average page occupancy, internal and external fragmentation, space
reclaimed after `delete`.

**Network** (`loopback` and `remote_*` targets) — bytes and frames sent/
received, bytes per object on the wire, syscalls, base RTT measured before
the load, compression ratio when enabled.

**Quality** — errors, retries, timeouts, cancellations, aborted
transactions, and the logical hash that proves equivalence between
variants.

**Per-window series** — every phase lasting longer than 30s emits a
`progress_window` on every fixed window (default 10s) with rate, latency,
RSS and file size for that window. Without this, temporal degradation is
invisible.

**Implemented in Subphase F.** In practice the cut is "at least one
`window_interval` window closed" (a phase shorter than the interval emits
no window at all, not even a tail one) — simpler than pre-measuring the 30s
and with the same effect: short phases never produce a `progress_window`.
`case_summary` carries
`windows{first_ops_per_second,last_ops_per_second,slope_ops_per_second_per_min,first_p99_ns,last_p99_ns}`
(from the case's first phase that closed any window) or `null` when none
closed; the rollup (§13.3) passes this field through as recorded.

## 9. Validation

Outside the measured region, at the end of every phase and every case:

1. object count matches the phase's expected value;
2. the set's logical hash matches the dataset's expected hash;
3. after `delete` phases, no removed id resolves and the count is as
   expected;
4. after `update`, a deterministic object sample is read and compared
   field by field;
5. reopening the database at the end of the case, repeating (1) and (2),
   when durability is part of the case;
6. `database_check` at the end of every mutation case at scale ≥ `100k`;
7. on remote targets with a replica, the follower's `applied_lsn` reaches
   the `primary_commit_lsn` and the follower's logical hash matches.

Any mismatch marks the case as `failed` and the whole campaign as `failed`.
No rate is ever published for a logically incorrect phase.

## 10. Resource budget

Every case declares, before running: object count, peak disk bytes, peak
memory and estimated duration. `run` checks free space and aborts with a
clear message before starting, instead of filling the disk halfway through
1M.

Flags: `--max-duration`, `--max-disk-gb`, `--max-rss-mb`. A case exceeding
the budget produces a `skipped_budget` record with the reason and the
estimated value. A campaign with skipped cases ends `partial`, never
`completed`.

Calibration table, to be filled in by measurement in Subphase A and
versioned in the repository (one file per platform):

| workload | payload | bytes/object | objects/s (100k) | peak disk 1M | duration 1M |
|---|---|---|---|---|---|
| `create_only` | `normal` | 458 | 6,288 | 458,000,000 | 159 s (extrapolated) |
| `create_delete_forward` | `normal` | 458 | 5,555 | 458,000,000 | 180 s (extrapolated) |
| `create_delete_reverse` | `normal` | 458 | 5,584 | 458,000,000 | 179 s (extrapolated) |
| `create_delete_interleaved` | `normal` | 458 | 5,323 | 458,000,000 | 188 s (extrapolated) |
| `crud_full` | `normal` | 1,674 | 413 | 1,674,000,000 | 2,420 s (extrapolated) |

Extrapolation from 10k to 1M is simple linear by default and marked as an
estimate; after the first real `1M` run, the measured value replaces the
extrapolation.

**Implemented in Subphase H (reduced calibration).**
`loadtests/calibration/windows-x86_64.json` carries real measurements at
`10k` and `100k` for the 5 implemented workloads (`payload=normal`);
`1k`/`250k`/`500k`/`1M` are simple linear extrapolation from the `100k`
point, marked per entry (`extrapolation_caveat`). Throughput dropped
between 2.4x (`create_only`) and 5.1x (`crud_full`) just going from `10k`
to `100k` — a real non-linear engine behavior at growing scale, not a
measurement artifact — so the `250k`/`500k`/`1M` values above are known to
be **optimistic** (the real duration tends to be larger). Replacing them
with real measurement at those larger scales remains future work.
`loadtests/calibration/linux-x86_64.json` doesn't exist yet (no Linux
environment available for this calibration round) — `estimate_case` simply
returns `known=false` for that platform, the same behavior as before
Subphase H.

`budget.cpp`/`campaign.cpp` use this table as the source of truth:
`estimate_case` looks up `loadtests/calibration/<platform>-<arch>.json`
(resolved at compile time); a case with a known estimate that exceeds
`--max-duration`/`--max-disk-gb`/`--max-rss-mb` produces `skipped_budget`
and is skipped (the campaign ends `partial`); before starting, `run` sums
the peak disk of every case with a known estimate and aborts with a clear
message if free space in `--work-dir` is insufficient.

## 11. Remote execution

Flow, evolving the current `scripts/run-remote-benchmark.ps1`:

1. host, user and remote path come from the registered environment (§4.4,
   `--environment ID`), resolved via `loadtests/environments.json` — no
   longer constants in the script. **Already implemented**: the current
   script accepts `-Environment <id>` and rejects an environment whose
   `kind` isn't `ssh`;
2. validating that the binary is an ELF before sending, as today;
3. validating free space on the remote host before starting;
4. running `modb_load run` with the same selectors used locally;
5. copying back **exactly one** file per campaign;
6. `.partial` preserved when the run fails, and resumable via `resume`;
7. printing the local path, size, `run_id`, status and SHA-256;
8. no password, token or user recorded in the result; never overwrites an
   existing file.

For `remote_client_local`, base RTT and observed bandwidth are measured
before the load and recorded in `environment`; without this, network
numbers aren't comparable across runs.

**Implemented in Subphase I (partial).** `scripts/run-remote-load.ps1`
covers items 1, 2, 4, 5, 7 and 8 (item 6 comes for free from Subphase F's
`resume`, which already works over any `.partial`, remote or not; item 3's
free-space check is Subphase H's already-implemented `run_campaign` check,
but it measures free space in the executing host's LOCAL `--work-dir` —
when the target is remote, that already runs on the remote host itself, so
the check is valid, it just hasn't been tested against a real host yet).
The `remote_colocated` target reuses `loopback`'s dispatch with no new
code at all (§4.3: the only difference between the two is WHERE the binary
runs). `remote_client_local` remains **without dispatch** — item 3 above
(RTT/bandwidth) has nowhere to be measured without it.

Honesty about what wasn't verified: the only registered `kind=ssh`
environment (`linux-remoto`) had an SSH key different from the one
registered in `known_hosts` at the time of this subphase (an OpenSSH
security warning, not bypassed on purpose — it could be a reprovisioned
host or something worse, and this is not a decision this agent should make
alone). The script was written closely following the pattern already in
production in `run-remote-benchmark.ps1` and validated locally as far as
possible without a network (environment resolution, `kind=ssh` selection,
ELF binary check) — **the full SSH round trip has never actually run**.
Before first real use: resolve the host key warning (deliberately, not
with `-o StrictHostKeyChecking=no`) and build `modb_load` for Linux.

## 12. Result format

UTF-8 JSON Lines, one object per line, same header as the benchmark plan
with its own schema:

```json
{"schema":"modb.loadtest","schema_version":1,"record":"...","run_id":"...","sequence":1}
```

| `record` | content |
|---|---|
| `run_start` | timestamp, command, profile, effective selectors, seed |
| `environment` | same as the benchmark plan, plus target, host, base RTT and bandwidth |
| `case_plan` | final case set and estimated budget (one line per campaign) |
| `case_start` | `case_id` and every effective parameter |
| `phase_start` | phase, expected objects, cache policy |
| `progress_window` | time window with rate, latency, RSS and file size |
| `phase_summary` | phase statistics, histogram and space metrics |
| `case_summary` | case aggregate, validations run and verdict |
| `case_error` | preparation, execution or validation error |
| `skipped_budget` | case not run due to budget, with the reason |
| `run_note` | interference or observation |
| `run_end` | duration, counts, status and previous content's hash |

File naming, the `.partial` → `.jsonl` policy, big-integer handling, units
in metric names and the ban on secrets follow §4 of
[PLANO_BENCHMARKS.md](PLANO_BENCHMARKS.md), with no divergence.

**Implemented in Subphase A/B**: `run_start`, `environment`, `case_plan`,
`case_start`, `phase_start`, `phase_summary`, `case_error`, `case_summary`,
`run_end` — enough for `create_only` to produce a valid, complete file.
**Implemented in Subphase F**: `progress_window` (every `window_interval`,
default 10s, only on phases that actually close a window) and `resume`.
**Implemented in Subphase H**: `skipped_budget`, emitted when a case with a
calibrated estimate exceeds `--max-duration`/`--max-disk-gb`/
`--max-rss-mb`. `run_note` as the cases that require it (environment
interference, §17 risk 11) come up.

The `case_id` field is also recorded as `scenario_id`, so `modb_bench
compare` works over load files with no change to the comparator. **Not yet
implemented**: `modb_bench compare` today only reads lines with
`"record":"scenario_summary"` (`benchmarks/runner/campaign.cpp:145`), and
this subphase's `modb_load` emits `case_summary` (a different record, by
design — see the table above). Making the two bridges talk to each other
is Subphase J's work (along with `gate`), not A/B's; until then, comparing
load campaigns requires reading the JSONL directly.

The campaign file is the primary source of truth for **one run**. The
series over time is derived from it and lives elsewhere (§13); no
historical analysis depends on keeping every raw file available.

The word "environment" names three distinct things in this plan —
deliberately related, never the same field:

| term | is | lives in |
|---|---|---|
| registered environment (D4, §4.4) | registered identity (`desktop-windows`, `linux-remoto`) — where the command runs | `loadtests/environments.json`; the `environment` field in the rollup |
| `environment` record (this section) | the actual hardware/OS/network of **one run** | one line per raw campaign |
| `env` (rollup, §13.3) | a summary of hardware/build, derived from the record above | inside every rollup |

The registered environment **resolves** `host_class` (§13.4) and feeds the
campaign's `environment` record; it doesn't replace it — the record still
carries what was actually observed (kernel version, measured RTT, etc.),
not just what was registered.

## 13. Historical series

### 13.1 The problem to solve

One file per campaign is not a series. Today `benchmark-results/` is
git-ignored, and the one preserved campaign (`benchmark-results-10f/`) was
committed by hand — that doesn't scale, isn't queryable, and doesn't
survive a machine change. Without an explicit historical layer, slow drift
(1% per week) is invisible: every pairwise comparison passes the
thresholds and the product silently degrades.

### 13.2 Two layers, distinct roles

| layer | where | content | lifecycle |
|---|---|---|---|
| raw | `load-results/` (git-ignored; local or on the remote host) | full campaign: windows, histograms, samples | immutable; retention by policy (§13.8) |
| historical | `load-history/` (versioned in Git) | one *rollup* per (case, run), no windows or histograms | append-only; never deleted |

The historical layer is small by construction — tens of bytes per case per
run — and so it can live in the repository, be diffable, reviewable in a
PR, and travel with the commit that produced it. The raw layer is large (a
`soak` campaign with 10s windows produces megabytes) and disposable as long
as the rollup survives.

Optional layer: a mirror of the raw files in external storage addressed by
SHA-256, for when an old run's detail needs to be recovered. The rollup
keeps the raw file's name and hash precisely to make this possible without
keeping everything locally.

### 13.3 Rollup record

One JSON object per line in `load-history/series.jsonl`, its own schema:

```json
{"schema":"modb.loadtest.rollup","schema_version":1,"series_key":"...","case_id":"...","run_id":"...","started_at":"..."}
```

Required fields, grouped by role:

- **temporal identity** — `run_id`, `case_id`, `started_at` in UTC
  ISO-8601 with milliseconds, `repeat_index` when `--repeat > 1`;
- **code provenance** — full `commit`, `branch`, `tree_dirty`, `diff_hash`
  when dirty, `workload_version`, `dataset_id`, `dataset_version`, `seed`;
- **comparability** — `series_key` and `series_key_version` (§13.4);
- **registered environment** — `environment`: the §4.4 `id` (e.g.
  `desktop-windows`, `linux-remoto`) — where the case ran, not to be
  confused with `env` below;
- **summarized environment** (`env`) — anonymized `host_id`, `host_class`,
  OS and version, architecture, CPU model, physical/logical cores, RAM,
  filesystem, device class, build type, compiler and version, sanitizers,
  page size, format and protocol versions — resolved from the registered
  environment and what was observed during the run;
- **per-phase metrics** — for each phase: operations, duration, ops/s, p50,
  p95, p99, p99.9, bytes per object, database and WAL size at the end,
  peak RSS, pages read/written/reused, errors;
- **case aggregates** — total duration, peak disk, peak RSS, space
  reclaimed after `delete`, write/space amplification;
- **temporal slope** (cases with windows) — rate and latency of the first
  and last window, and the simple regression slope between them: this is
  what makes intra-run degradation visible in the historical series;
- **verdict** — `status`, list of validations run, `comparable`;
- **traceability** — the raw file's name and its SHA-256.

A rollup missing `commit`, `series_key`, `environment`, `host_class`, build
type, `seed` or `status` is **rejected** by the indexer with an error. A
historical point with no provenance is noise that poisons the series years
later; rejecting it at the door is cheaper than cleaning up afterward.

Canonical field names — the dashboard (§13.11) reads exactly these:

```json
{"schema":"modb.loadtest.rollup","schema_version":1,
 "series_key":"a1b2c3d4e5f60718","series_key_version":1,
 "case_id":"load.create_only.embedded.100k",
 "workload":"create_only","target":"embedded","scale":"100k","objects":100000,"variant":"",
 "environment":"linux-remoto",
 "run_id":"019f2c...","started_at":"2026-08-07T03:12:44.120Z","repeat_index":0,
 "commit":"a1b2c3d4...","commit_short":"a1b2c3d","branch":"master","tree_dirty":false,"diff_hash":null,
 "workload_version":1,"dataset_id":"user_v1","dataset_version":1,"seed":"123456",
 "env":{"host_id":"h7c1a2","host_class":"bench-linux-01","os":"Linux 6.8.0","arch":"x86_64",
        "cpu_model":"AMD EPYC 7443P","cores_physical":8,"cores_logical":16,"ram_gb":32,
        "fs":"ext4","device_class":"nvme","build_type":"Release","compiler":"gcc 14.2",
        "sanitizers":"none","page_size":4096,"format_version":1,"protocol_version":1},
 "params":{"payload":"normal","batch":1000,"concurrency":1,"readers":0,
           "durability":"sync_real","cache":"warm","primary_storage":"full"},
 "phases":[{"phase":"create","operations":100000,"duration_ns":763000000,
            "ops_per_second":131062,"latency_ns":{"p50":5490,"p95":21400,"p99":33600,"p999":77200},
            "bytes_per_object":271,"db_bytes":36180000,"wal_bytes":29800000,
            "peak_rss_bytes":132120576,"pages_read":31000,"pages_written":42000,
            "pages_reused":0,"errors":0}],
 "totals":{"duration_ns":763000000,"peak_disk_bytes":69680000,"peak_rss_bytes":132120576,
           "reclaimed_bytes":0,"write_amplification":2.44,"space_amplification":1.35},
 "windows":{"first_ops_per_second":135000,"last_ops_per_second":128400,
            "slope_ops_per_second_per_min":-390,"first_p99_ns":30100,"last_p99_ns":34200},
 "status":"completed","comparable":true,
 "validations":["count","logical_hash","reopen","database_check"],
 "raw_file":"modb-load-20260807T031244Z-a1b2c3d-bench01.jsonl","raw_sha256":"9f81c2…"}
```

Missing fields are `null`, never an invented zero; units are part of the
name (`_ns`, `_bytes`, `ops_per_second`), same as the benchmark plan.

### 13.4 `series_key` — what can be compared with what

`series_key` is a stable hash over the set of attributes that must be
identical for two points to belong to the same series:

```text
case_id, workload_version, dataset_id, dataset_version,
effective semantic parameters (scale, payload, batch, concurrency,
  durability, cache, primary_storage),
build class, architecture, page size, format version, protocol version,
host_class, execution target
```

Rules:

1. a semantic change in the workload or dataset bumps its version, which
   produces a **new series** — it never mixes with the old one;
2. changing machine, compiler or build class also produces a new series;
   the report shows the discontinuity explicitly instead of stitching two
   incomparable series together;
3. `series_key_version` tracks the hash formula, so a change to the
   definition itself can be recomputed over existing rollups without
   losing history;
4. `host_class` is a configured label (`dev-windows`, `bench-linux-01`),
   not the hostname — this allows swapping equivalent hardware without
   breaking the series on purpose, and the discontinuity gets recorded in
   `run_note`. Since §4.4, `host_class` is resolved from the registered
   environment, not typed by hand; **the `environment` (id) itself does
   not enter the hash** — two registered environments with the same
   `host_class` (equivalent hardware) stay in the same series on purpose,
   and that is the intent.

### 13.5 Indexing

```text
modb_load index [--scan DIR] [--history load-history/series.jsonl] [--dry-run]
```

Reads raw campaigns (final `.jsonl` files and `.partial`s), extracts one
rollup per case, and appends to the historical file. Required properties:

- **idempotent** — dedup by (`run_id`, `case_id`, `repeat_index`);
  reindexing the same directory doesn't duplicate points;
- **append-only** — never rewrites an existing line; corrections happen
  via a new line with `supersedes` pointing to the corrected `run_id`;
- **ordered by `started_at`**, never by file modification date;
- **failed cases enter the series** with `status=failed`; erasing failure
  from history is the most efficient way to repeat the same mistake;
- `--dry-run` prints what would be appended, with the rejection reasons.

The indexer runs at the end of `run` by default (`--no-index` turns it
off) and can also run manually over raw files brought back from a remote
run.

### 13.6 Querying

```text
modb_load trend  --case ID [--metric ops_per_second] [--phase create] [--last N] [--since DATE]
modb_load trend  --series-key HASH [...]
modb_load report --format md|csv [selectors]   # exports the series for external analysis
```

`trend` prints one line per point: UTC date, short commit, value, delta
against the previous point, delta against the median of the last K
(default 5), and an outlier mark. Reading rules built into the tool:

- fewer than 3 points in the series → prints the data and refuses to emit
  a verdict;
- the reference is the **moving median**, not the isolated previous point,
  which is noise;
- points with `comparable=false` appear in the listing and are excluded
  from the calculation;
- a `series_key` break is printed as an explicit separator line.

`report` exists because no internal tool will ever cover every future
analysis: CSV and Markdown let you take the series into a spreadsheet,
notebook or chart without depending on the binary.

### 13.7 Point-in-time regression and slow drift

Two distinct mechanisms, because they catch distinct things:

| mechanism | compares | catches |
|---|---|---|
| per-run gate | candidate × median of the last 5 in the same series | abrupt regression introduced by a commit |
| slow drift | median of the last 5 × median 20 points back | 1–2% degradation per run that no point gate would flag |

Thresholds inherited from §11 of [PLANO_BENCHMARKS.md](PLANO_BENCHMARKS.md),
with no divergence: alert at 5% of the median/throughput, fail at 10%, p99
at 15%, space/WAL/network bytes at 10%, and any correctness divergence is
an immediate failure. For drift, the threshold is cumulative: 15% between
the two medians.

`modb_load gate --case ID` returns an exit code usable in CI. No gate
decides with a series shorter than 3 points: it returns
`insufficient_history` and success, so as not to block on missing data.

**Implemented in Subphase J.** `modb_load gate --case ID --metric NAME
[--phase NAME] [--history-file PATH]` reuses `compute_trend` (Subphase C)
without duplicating the moving median: the per-run gate IS the last
point's verdict (`compute_trend` already compares the candidate against
the median of up to the 5 previous points in the same series);
`status != "completed"` on the last point becomes `fail` directly, never a
threshold comparison (§9: a correctness divergence is an immediate
failure). Slow drift is a new calculation
(`loadtests/history/gate.cpp`) over the same window of comparable values:
median of the last 5 (including the candidate) vs. median of the up to 5
ending 20 runs back, a single 15% threshold, no alert tier. Exit code: 0
when `passed` (includes `insufficient`/`insufficient` — doesn't block CI
for lack of history), 1 when the point gate OR the drift check fails.
Verified with synthetic data: a 12% point-in-time drop (`ops_per_second`,
10% threshold) fails the per-run gate; 23 runs dropping 1 unit each (~20%
cumulative) fail the drift check even with every isolated point gate
passing (each step stays well below the 5% alert threshold).

### 13.8 Retention

Raw files: keep the last N per series (default 10), all baseline-marked
ones, all with `status=failed`, and all with an interference `run_note`.
Compress anything over 30 days old. Removal only via `--prune --confirm`,
and the removed raw file's rollup stays — with its hash, so the absence is
detectable.

Rollups: never deleted, never rewritten. They are the project's memory.

**Implemented in Subphase J (partial).** `modb_load prune [--keep N]
[--confirm] [--history-file PATH] [--baselines-file PATH] [--raw-dir DIR]`
groups `series.jsonl`'s rollups by `series_key`, keeps the `--keep`
(default 10) most recent by `started_at`, and among the older ones only
removes the `raw_file` (never the rollup line) of entries that aren't
`status=failed` and don't have their `run_id` marked in `baselines.json`.
Without `--confirm`, it only lists what would be removed. **Not
implemented**: compression past 30 days, and protection by interference
`run_note` — `run_note` itself has never been emitted by any real campaign
yet (no subphase has implemented that record type), so the rule has no
practical effect until it exists.

### 13.9 Marked baselines

`load-history/baselines.json` maps `series_key` → an explicitly chosen
`run_id`, with a date and a text reason. A baseline is a recorded human
decision, not "the oldest run" or "the best one". Immutable: replacing a
baseline means appending a new entry with the reason for the change.

**Implemented in Subphase J.** `modb_load baseline --case ID --run-id ID
--reason TEXT [--history-file PATH] [--baselines-file PATH]` resolves the
`series_key` by looking up the (`case_id`,`run_id`) pair in `series.jsonl`
and appends an entry (never rewriting previous ones) to
`load-history/baselines.json`.

### 13.10 Anonymization

`host_id` is a hash of the hostname with a locally configured salt
(`MODB_LOAD_HOST_SALT`), never the raw hostname. No user, token, client IP
address or real path enters the rollup; paths are normalized. The
`environment` field records only the registered `id` (`linux-remoto`),
never `connection.host`/`connection.default_user` from
`loadtests/environments.json` — this distinction exists precisely so the
rollup never needs to carry any connection detail. The historical series
is versioned in Git — what goes into it is public to everyone with the
repository.

### 13.11 Dashboard

`loadtests/dashboard/index.html` — a single file, no dependencies, that
opens via `file://` and reads `series.jsonl` in the browser itself (nothing
is sent to any external service). Implemented; consumes the §13.3 schema
with no conversion.

Input: a file picker, drag-and-drop, or `?src=…` when the folder is served
over HTTP. One button loads a labeled synthetic series, to inspect the
panel's readings before any real measurement exists.

**How to actually open it**, after running `modb_load run`/
`scripts/run-load.ps1`:

```powershell
Invoke-Item .\loadtests\dashboard\index.html    # opens in the default browser
```

```bash
open loadtests/dashboard/index.html             # macOS
xdg-open loadtests/dashboard/index.html         # Linux
```

With the page open, drag `load-history/series.jsonl` onto the load area
(or click "Open series.jsonl…" and browse to the file) — the cases from
whichever campaign(s) were already indexed show up in the filters and
charts. **It only accepts lines with `"schema":"modb.loadtest.rollup"`**
— the raw campaign file (`load-results/modb-load-*.jsonl`, schema
`modb.loadtest`) is rejected line by line as "unknown schema" if opened
directly; it needs to go through `modb_load index`/`run` (which already
indexes by default, `--no-index` turns it off) to become a rollup in
`series.jsonl` before opening it in the panel.

The panel is a visual reading of this chapter's rules, not decoration:

| element | rule it makes visible |
|---|---|
| trend with moving median and drawn alert/fail thresholds | §13.7: the point is compared against the median of the last 5, not the previous one |
| vertical `series_key` separator | §13.4: incomparable series aren't stitched together; the median resets at the break |
| per-point verdict mark + drift pin | §13.7: the per-run gate and slow drift are distinct mechanisms |
| scale chart with a constant cost-per-object reference | §4.1: is 10k → 1M linear or super-linear? — restricted to the same workload, target and environment as the selected case, otherwise it mixes hardware |
| per-phase composition | §4.2: each phase measured separately |
| "Environment" filter and matching table column | §4.4: separates runs by registered environment without mixing distinct hardware in any chart |
| table with Δ previous, Δ median and CSV export | palette contrast and §13.6: no value exists only in the chart |

Built-in reading rules, same as the CLI: fewer than 3 points gets no
verdict, points with `comparable=false` appear but are excluded from the
calculation, failures appear in red instead of vanishing, and "today" is
the series' most recent point — not the machine's clock, so old history
stays legible.

## 14. Artifacts to implement

```text
loadtests/
  environments.json               registered-environment catalog (implemented, §4.4)
  json_value.hpp/.cpp             implemented -- minimal read-only JSON parser (there was
                                 nothing beyond serialization in benchmarks/runner)
  modb_load.cpp                  implemented -- CLI: run, list-cases, list-profiles,
                                 index, trend, report, resume, gate, baseline, prune
  matrix.hpp/.cpp                implemented -- dimensions, expansion, selectors, ids
  environments.hpp/.cpp          implemented -- loads/validates environments.json, resolves --environment
  profiles.hpp/.cpp              implemented -- the 8 profiles from §6.2
  budget.hpp/.cpp                implemented -- --accept-unknown-budget gate; calibration table (Subphase H)
  campaign.hpp/.cpp              implemented -- resolve_cases, render_case_plan,
                                 run_campaign, resume_campaign (the §12 records)
  dataset_user.hpp/.cpp          implemented -- user_v1 generator, a pure function of (seed,index)
  target.hpp                     implemented -- shared structs (PhaseMetrics, WorkloadParams, CaseRunResult)
  target_embedded.hpp/.cpp       implemented -- against a real modb::object::Database
  target_client.cpp              implemented via net::Client (loopback/remote)
  history/
    rollup.hpp/.cpp              implemented -- campaign -> rollup extraction
    series_key.hpp/.cpp          implemented -- comparability hash (environment
                                 doesn't enter; host_class does)
    index.hpp/.cpp               implemented -- idempotent append, rejects rollups
                                 with no provenance
    trend.hpp/.cpp               implemented -- 11 metrics (same registry as the
                                 dashboard), moving median, series break, verdict
    gate.cpp                     implemented -- point regression and slow drift (Subphase J)
    retention.hpp/.cpp            implemented -- prune (Subphase J)
    baselines.hpp/.cpp            implemented -- marked baselines (Subphase J)
  dashboard/
    index.html                   historical-series panel (implemented, §13.11)
  workloads/
    create_only.hpp/.cpp            implemented
    create_delete_forward.hpp/.cpp  implemented
    create_delete_reverse.hpp/.cpp  implemented
    create_delete_interleaved.hpp/.cpp implemented -- stride=7, same pattern as
                                       benchmarks/scenarios/object_store_lifecycle.cpp
    crud_full.hpp/.cpp              implemented -- 6 phases, deterministic sample per
                                   update (§9 item 4), not the whole set
    read_hotspot.cpp                implemented -- behavior workload (§4.2.1)
    range_scan_sweep.cpp            implemented -- behavior; formalizes the old crud_query
    mixed_oltp.cpp                  implemented -- behavior; concurrency as a workload, not just a dimension
    snapshot_hold.cpp               implemented -- behavior
    blob_lifecycle.cpp              implemented -- behavior
    cascade_delete.cpp              implemented -- behavior; formalizes the old crud_relationships
    oversubscribed_churn.cpp        implemented -- behavior
    restart_recovery.cpp            implemented -- behavior; failpoint-based kill simulation (§17)
  dataset_user_blob.hpp/.cpp        dataset variant with a blob, only for blob_lifecycle
  calibration/
    windows-x86_64.json             implemented -- reduced calibration (Subphase H)
    linux-x86_64.json                not implemented yet -- no Linux environment available
  config/
    load-local.yaml                example config for run-load.* (implemented, §6.5)
    load-smoke.yaml                 smoke-scale config (implemented)
scripts/
  run-remote-benchmark.ps1        already consumes loadtests/environments.json (implemented)
  run-load.ps1 / run-load.sh      read YAML and call `modb_load run` locally (implemented, §6.5)
  run-remote-load.ps1             partial (Subphase I) -- see §11
tests/
  load_matrix_test.cpp           implemented (`ctest -R modb.load_matrix`) -- expansion,
                                 selectors, ids, empty set
  load_workload_test.cpp         implemented (`ctest -R modb.load_workload`) -- every
                                 workload at a tiny scale, invariants
  load_history_test.cpp          implemented (`ctest -R modb.load_history`) --
                                 stable series_key, idempotent index, rejection
                                 of rollups with no provenance, moving median/verdict,
                                 series break resets the window, gate, retention, baselines
  load_campaign_test.cpp         implemented (`ctest -R modb.load_campaign`) --
                                 resume, budget skip, calibration lookup
load-history/                    versioned in Git
  series.jsonl                   append-only, one rollup per (case, run)
  baselines.json                 series_key -> chosen run_id, with a reason
load-results/                    git-ignored
```

Direct reuse, no copying: `benchmarks/runner/jsonl_writer`, `environment`,
`sha256` and `json_util`. **Implemented as option (a)**: `modb_load_core`
(CMakeLists.txt) links `modb_bench_core` `PUBLIC` instead of duplicating
the four files — inherits the `benchmarks/` include dir for free, so
`#include "runner/jsonl_writer.hpp"` works in `loadtests/*.cpp` with no
extra configuration. Extracting a standalone `bench_runner_core` remains
possible later, but wasn't necessary for A/B.

The key design piece is `target.hpp`: a CRUD interface over `User` with two
implementations. The workload and the matrix don't know whether they're
embedded or over the network — that's what lets the same case run on all
four targets.

## 15. Implementation order

One subphase per branch, per the project's convention.

> **Current status and revised sequence**:
> [docs-process/PLANO_IMPLEMENTACAO_CARGA.md](../docs-process/PLANO_IMPLEMENTACAO_CARGA.md)
> surveys what is actually implemented (by reading the code, not this doc),
> groups the rest into dependency-driven waves, and tracks progress
> subphase by subphase. It records two divergences from this table, both
> justified there: two debts from what was already implemented (`case_id`
> that lies about concurrency; metrics the dashboard assumes but the
> collector doesn't produce) come before any new subphase, and Subphase H
> (high scales) is moved ahead of G/I (network), because the "10k to 1M"
> question is only answerable with `embedded`.

| subphase | delivers | acceptance criterion |
|---|---|---|
| A | **Implemented.** matrix, ids, selectors, all 8 §6.2 profiles, `list-cases`, `--dry-run`, budget with no calibration (gate `--accept-unknown-budget`), `environments.hpp/.cpp` (validates `--environment` against `loadtests/environments.json`), `json_value.hpp/.cpp` (minimal JSON parser, there was nothing beyond serialization) | `load_matrix_test` green (13 cases); `list-cases`/`run --dry-run` print without running; an invalid `--environment` fails with the list of registered ids; `run-load.ps1`/`run-load.sh` stop printing "not found" and call the real binary |
| B | **Implemented.** `campaign.cpp` (writer + the 8 §12 records used so far), `dataset_user` (a pure (seed,index) generator, splitmix64), `target_embedded.cpp` (against a real `modb::object::Database` — batched create, `User` binding, commit), the `create_only` workload, scales `1k`/`10k`/`100k`/`1M` (the whole catalog, not just 1k/10k) | `modb_load run --profile load-smoke --workload create_only --accept-unknown-budget` produces a valid JSONL with `hash_match:true` (create + a full reread compared by SHA-256); profiles with an unimplemented workload report a clear per-case `case_error` and `status:"partial"`, never hanging the campaign |
| C | `series_key`, rollup, idempotent `index`, `trend`, `report` | two `load-smoke` runs produce two points in the same series; reindexing doesn't duplicate; the dashboard (§13.11) opens the generated `series.jsonl` with no conversion |
| D | `create_delete_forward`, `create_delete_reverse`, `create_delete_interleaved` | zero-count invariants and recorded reclaimed space |
| E | `crud_full` with the six phases separated | field-by-field read matches; per-phase `phase_summary` |
| F | `progress_window`, rollup slope, `case_summary`, `resume` | an interruption at 100k is resumable without rerunning a completed case |
| G | `target_client`, `loopback` target, network metrics | `load-local` covers embedded and loopback with the same case |
| H | `250k`/`500k`/`1M` scales, measured calibration, active guard rails | §10's table filled in; a case over budget is skipped with a record |
| I | `remote_colocated`, `remote_client_local`, indexing raw files brought back (`run-remote-benchmark.ps1` already resolves `-Environment` from the catalog, §4.4) | `load-remote` brings back exactly one file from the host, with a hash, and it enters the series |
| J | `gate`, slow drift, retention and `--prune`, marked baselines | a synthetic 12% regression and a synthetic 15% drift are detected |
| K | secondary dimensions, pairwise `load-heavy`, `load-soak` | every non-default value exercised at least once |
| L | `read_hotspot`, `range_scan_sweep` (index over `dataset_user`) | hit rate and plan (index/scan) recorded; per-selectivity count matches |
| M | `mixed_oltp` over concurrent sessions (reuses `--concurrency`, §4.5) | final count reconciles under real concurrency; sample checksum matches |
| N | `snapshot_hold` | reading through the open snapshot stays identical throughout the churn; retained versions/bytes recorded |
| O | `dataset_user_blob`, `blob_lifecycle` | byte-for-byte blob hash matches; space reclaimed after delete |
| P | `cascade_delete` over `Ref`/`OwnedRef` hierarchies | zero orphan refs at configurable depth/width |
| Q | `oversubscribed_churn`, `load-behavior` profile | same invariants as `create_delete_interleaved` plus a recorded eviction ratio; the profile runs end to end |
| R | kill/restart harness (Windows and Linux), `restart_recovery` | post-recovery hash == last durable commit's hash; recovery time recorded on both OSes |

`schema_evolution` and `replica_catchup` (§4.2.1) have no subphase of their
own: they enter the order only after the infrastructure they depend on
(simultaneous `Binding` versioning; a harness-orchestrated replica) exists
for another reason — it's not worth building that infrastructure just for
the load test.

A and B are the minimum useful slice: they already deliver "run just a
subset" with one real workload. **C comes before the other workloads on
purpose**: from it on, every run already deposits a historical point, so
no measurement taken between C and K is lost. If history were the last
subphase, all the work from D to I would produce numbers nobody could
compare later.

## 16. Acceptance criteria

The plan will be implemented when:

- `modb_load list-cases --profile load-standard` prints the matrix with an
  estimated budget without running anything;
- `modb_load run --workload create_delete_reverse --scale 100k --target embedded`
  runs exactly one case and produces a single final JSONL;
- every D1 × D2 × D3 × D4 combination declared in the profiles is runnable
  in isolation via selectors, with no code changes;
- registering a new environment (`kind=local` or `kind=ssh`) in
  `loadtests/environments.json` and running/filtering by it requires only
  editing that file — never `modb_load`'s code or the scripts';
- interrupting a campaign leaves a readable `.partial`, and `resume`
  finishes the rest without repeating completed cases;
- every workload validates its invariants, and an injected corruption
  results in `failed`, never a published number;
- the `10k` to `1M` scales are covered by real measurement at least once
  on Windows and on Linux, with the calibration table filled in;
- a case over the disk or time budget is skipped with an explicit record,
  and the campaign ends `partial`;
- a remote campaign brings back exactly one file, with `run_id`, status
  and SHA-256 printed, and no secret in the content;
- three runs of the same case in the same environment stay within the
  declared variation for gated cases.

Specifically for the additional workloads (§4.2.1):

- `read_hotspot` records the cache hit rate, and read values match the
  expected ones field by field;
- `range_scan_sweep` returns the expected count at every scan selectivity
  and records whether the plan used was an index or a full scan;
- `mixed_oltp` reconciles the final count (created − removed) under real
  concurrency, with a deterministic sample checksum matching — no write
  lost under contention;
- `snapshot_hold` produces identical reads through the open snapshot from
  the start to the end of the churn, and records retained versions,
  retained bytes and the GC pause on close;
- `blob_lifecycle` matches the read blob's byte-for-byte hash against the
  written one in at least one 256 MiB size, with space reclaimed after
  delete;
- `cascade_delete` leaves no orphan ref after removing the root of a
  hierarchy with configurable depth and width, with total removed == total
  created;
- `oversubscribed_churn` degrades measurably (doesn't hang or corrupt) once
  volume exceeds the configured cache, with a recorded eviction ratio;
- `restart_recovery` produces a post-recovery hash identical to the last
  durable commit's at at least three kill points (mid-transaction,
  post-commit, mid-checkpoint), on Windows and on Linux;
- `schema_evolution` and `replica_catchup` remain documented as dependent
  on infrastructure not yet built, entering no profile until that
  infrastructure exists — the acceptance criterion is the dependency being
  explicit, not the implementation itself.

Specifically for the historical series:

- every run, local or remote, deposits a rollup in
  `load-history/series.jsonl` with full provenance, and an incomplete
  rollup is rejected with an error;
- `modb_load index` is idempotent: reindexing the same directory twice
  doesn't change the historical file;
- `modb_load trend --case load.create_only.embedded.100k` shows the full
  series with date, commit, value and delta against the moving median;
- a synthetic 12% regression is rejected by `gate`, and a synthetic 15%
  drift spread across 20 runs is detected by the drift mechanism even when
  no individual step triggers the gate;
- changing host, compiler or build class produces a new series and shows
  up as an explicit discontinuity in the report, never as a performance
  drop;
- removing raw files by retention doesn't prevent any historical analysis:
  the series stays complete and the raw file's absence is detectable via
  the hash;
- the historical file stays small enough to be versioned and reviewed in a
  PR (on the order of KB per month of regular use);
- the dashboard (§13.11) opens the real `series.jsonl` with no conversion,
  and shows the same reading as `modb_load trend`/`gate` for the same case
  and metric — if they diverge, one of the two is wrong.

## 17. Risks and open questions

1. **Meaning of "users"** — this plan assumes record volume (§2). If the
   target is concurrent sessions instead, D1 and concurrency swap roles;
   decide before Subphase A, because it changes the profiles, not the
   architecture.
2. **1M under `wal_only`** — with the primary as WAL-only
   ([ADR-017](decisions/ADR-017-primary-wal-only-sem-arquivos-de-dados.md)),
   1M objects become log growth; without a measured retention/checkpoint
   policy, the case measures the disk filling up. Keep `wal_only` out of
   the default profiles until Subphase K.
3. **Writes don't scale with sessions** — single-writer engine (Phase 5).
   `c4` and `c16` cases should be read as contention and fairness;
   document this in the result to avoid a wrong "doesn't scale"
   interpretation.
4. **Disk in `load-heavy`** — the heavy matrix at 1M can require dozens of
   GB; Subphase H's guard rail is a prerequisite for running it
   unsupervised.
5. **`remote_client_local` at high scale** — measures the link, not the
   database; deliberately limited to `10k`/`100k`.
6. **Duplication with benchmarks** — if `runner/` isn't extracted to a
   shared target in Subphase B, the two trees diverge. Extract at the
   first real need, not later.
7. **Toolchain** — the project's `cmake`/`mingw` live inside CLion;
   Subphase A must confirm the new target builds with today's preset
   before growing.
8. **Series too short to decide** — load at `1M` doesn't run on every
   commit, and a series with 3 points per quarter can't sustain a gate.
   Accepted consequence: the high scales produce a trend series (human
   analysis), and the automatic gate stays restricted to `10k`/`100k`,
   which run frequently. Decide each profile's cadence together with
   Subphase J, not after.
9. **Development machine in the series** — points collected on a work
   machine under concurrent load are noisy. `host_class` separates the
   series, but before §4.4 it was easy to forget to set it by hand and
   contaminate the official series. **Mitigated by the registered
   environment (§4.4)**: `host_class` is now resolved from the catalog,
   not typed per run; the residual risk is picking the wrong
   `--environment` (e.g. `linux-remoto` for a session that actually ran on
   the desktop) — with no automatic detection for this, it's an operator
   error, not a system one.
10. **Versioned rollup causes merge conflicts** — an append-only file
    touched by several branches conflicts. Mitigation: one line per point,
    ordering by `started_at` at read time rather than in the file, and
    conflict resolution by union of the lines — `index` detects duplicates
    by `run_id` regardless.
11. **Environment drift confused with regression** — an OS, firmware or
    disk-driver update on the same registered environment changes the
    baseline without changing `series_key` (the registered `host_class`
    doesn't change on its own). Mitigation: a mandatory `run_note` when the
    `environment` record (§11, the run's actual hardware/OS — not to be
    confused with the rollup's `environment` field) diverges from the
    series' previous point in a relevant field, and the report marks the
    point.
12. **Record vs. reality** — nothing prevents physically running on a
    different machine than the one `--environment` points to (e.g. SSH to
    a host that isn't the registered one). The catalog declares intent, not
    hardware identity; the actual check comes from the observed
    `environment` record (§11) diverging from what's expected for that
    `host_class` — which falls into risk 11.
13. **Kill/restart is platform-specific** — `restart_recovery` (§4.2.1)
    needs to kill the process at defined points (mid-transaction,
    post-commit, mid-checkpoint) in a way that doesn't mask what's being
    measured: on Linux, `SIGKILL`; on Windows, there's no direct
    equivalent (`TerminateProcess` isn't the same abrupt-failure
    semantics). Subphase R's harness needs a per-OS mechanism, documented,
    and both have to produce the same acceptance criterion — an identical
    post-recovery hash.
14. **Invariant under concurrency is more expensive to verify** —
    `mixed_oltp` mixes concurrent create/read/update/delete; "no write
    lost" can't be verified object by object without serializing (which
    defeats the workload's own purpose). Mitigation: a checksum over a
    deterministic id sample, not the whole set — cheaper, but it's
    sampling-based verification, not exhaustive; document this explicitly
    in the result.
15. **`blob_lifecycle` requires its own dataset** — the basic ladder's
    workloads use `dataset_user` (§7) with no blob. `dataset_user_blob`
    (Subphase O) is a new variant, not an extension of the existing one,
    because 256 MiB blobs in the default dataset would bloat every other
    workload that doesn't need them.
16. **`snapshot_hold` can mask a memory leak as expected retention** —
    versions retained while a snapshot is open are correct by design
    (MVCC); Subphase N needs to distinguish this from a real leak by
    comparing growth **after** `close_snapshot` against what's expected,
    not just during the churn.
