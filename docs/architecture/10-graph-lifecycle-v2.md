# 10 — Graph Lifecycle V2: Migration Notes (Phase 2)

Phase 2 changes how TuGraph manages graph storage. This document describes the
behaviour changes an operator or embedder must know about when upgrading from
the Phase 0/1 baseline.

## What changed

| Before (Phase 0/1) | After (Phase 2) |
|---|---|
| Every registered graph is opened at startup (LMDB env + thread + fds) | Only the **catalog** (name → config) is read at startup; graphs open lazily on first access |
| Memory scales with registered graph count | Memory scales with **max_open_graphs** (configurable, default 1,000) |
| File descriptors scale with registered graph count | File descriptors scale with **max_open_graphs** |
| Startup time scales with registered graph count | Startup reads only the catalog (metadata rows); graph envs are not touched |
| `MAX_NUM_GRAPHS = 4096` hard-coded limit | `max_graphs` runtime setting (0 = no application-level limit) |
| Graph stays open forever once created | Idle graphs are **evicted** (LMDB env closed, data on disk preserved) |
| One graph = one thread (LMDB validator) | One open graph = one thread; registered-but-closed graphs have no thread |

## New configuration settings

| Setting | Default | Description |
|---|---|---|
| `max_graphs` | 0 (unlimited) | Maximum number of REGISTERED graphs. Replaces the hard-coded 4096. |
| `max_open_graphs` | 1000 | Maximum number of graphs physically open at one time. When exceeded, the least-recently-accessed graph with no outstanding references is evicted. |
| `graph_idle_timeout_s` | 900 | Evict graphs idle longer than this many seconds. 0 disables idle eviction (only evict when the open count exceeds `max_open_graphs`). |
| `lmdb_notls` | true | Open LMDB environments with `MDB_NOTLS`. Required for more than ~1,000 graphs. |
| `lmdb_max_dbs` | 10000 | Max named tables per LMDB environment. Reducing this (e.g. 1024) reduces per-graph memory. |
| `monitor_host` | "" (disabled) | Prometheus scrape endpoint (`host:port`, e.g. `0.0.0.0:8080`). When set, the server exposes `/metrics` with the existing resource gauges plus `tugraph_graph_cache_*` lifecycle counters (see below). |

## New metrics

`CALL dbms.graph.cacheStats()` returns:

| Field | Description |
|---|---|
| `registered_graphs` | Number of registered graphs (from the catalog) |
| `open_graphs` | Number of graphs physically open (with a live LMDB env) |
| `cold_opens` | Number of lazy opens since server start |
| `cache_hits` | Number of GetGraphRef calls that found the graph already open |
| `cache_misses` | Number of GetGraphRef calls that triggered a cold open |
| `evictions` | Number of graphs evicted (closed + removed from the open set) |
| `evict_skipped_refs` | Number of eviction attempts skipped because the graph had outstanding references |

### Prometheus endpoint

When `monitor_host` is configured (e.g. `monitor_host=0.0.0.0:8080`), the
server exposes a Prometheus `/metrics` scrape endpoint. In addition to the
existing `resources_report_*` resource gauges, it publishes the
`tugraph_graph_cache` family (one time series per counter, carried in the
`metric` label):

```
tugraph_graph_cache{metric="registered_graphs"}
tugraph_graph_cache{metric="open_graphs"}
tugraph_graph_cache{metric="cold_opens"}
tugraph_graph_cache{metric="cache_hits"}
tugraph_graph_cache{metric="cache_misses"}
tugraph_graph_cache{metric="evictions"}
tugraph_graph_cache{metric="evict_skipped_refs"}
```

These are the same counters returned by `dbms.graph.cacheStats()`, scraped
live (refreshed every 5 s). The endpoint is disabled by default.

## Behavior changes

### 1. Startup no longer opens graphs

The server starts by reading the graph catalog (metadata rows in the meta
store). Graph storage environments are **not** opened. This means:

- Startup time is independent of the registered graph count (measured: 0.05 s
  for 10,000 graphs).
- The `default` graph's directory is not created until it is first accessed.
- A graph that was registered but never accessed has no `data.mdb` on disk.
  `create_if_not_exist = true` is set on lazy open, so the first access creates
  the store and writes the db secret.

### 2. Cold-open latency

The first query against a closed graph triggers a lazy open (~1 ms: LMDB env
open + table setup + db secret check + schema load). Subsequent queries on the
same graph are cache hits (no env open). Under mixed workloads, some queries
will be slower than others (cold vs cached).

### 3. Graph eviction

When the open count exceeds `max_open_graphs`, the least-recently-accessed
graph with no outstanding references is evicted (LMDB env closed, data on disk
preserved). A graph that is being accessed (has an outstanding
`AccessControlledDB`) is never evicted — the `ScopedRef` protects it. If ALL
open graphs are pinned, the open count temporarily exceeds `max_open_graphs`
and a warning is logged.

Graphs idle longer than `graph_idle_timeout_s` are also evicted by a
background task (even if the open count is below the limit), which frees
memory and file descriptors.

### 4. Backup and snapshot are sequential

`Galaxy::Backup`, `Galaxy::SaveSnapshot` and HA snapshot/restore now iterate
the graph catalog and open→backup→close each graph **sequentially**, rather
than opening all graphs at once. `GraphManager::Backup` keeps **at most one
graph physically open** at a time (it closes each graph via `CloseGraph`
immediately after backing it up), so a backup of N graphs no longer grows the
open set to `max_open_graphs`. `Galaxy::Backup` relies on the LRU eviction to
stay bounded at `max_open_graphs` during its pass.

This bounds resource usage, but means the backup of a very large graph
population takes proportionally longer.

### 5. HA raft-apply latency

In HA mode, replicated writes that reference a closed graph trigger a lazy
open inside the raft apply path. This adds ~1 ms per cold open. Under mixed
workloads (frequent cross-graph writes), this can increase raft apply latency.

### 6. `max_graphs` now counts registered graphs

In Phase 0/1, `max_graphs` counted **open** graphs (because all graphs were
open). In Phase 2, it counts **registered** graphs (from the catalog). The
open count is separately limited by `max_open_graphs`.

## Correctness defects (see 08)

See [08-correctness-findings.md](08-correctness-findings.md) for:

- F1: `UNWIND ... CREATE` under-insertion in the default Cypher v2 engine —
  **FIXED** (per-record `Visited` reset in the create operators)
- F2: label-filtered `count()` failing on empty labels — **FIXED** (empty-graph
  guard in `FindVertices` + single-row `0` from the count-traversal operators)
- F3: intermittent unit-suite SIGSEGV (pre-existing, not caused by Phase 2) —
  still open
- F3a: Phase 2 eviction-task use-after-free — **FIXED** (eviction task owned by
  `Galaxy`, resolves `graphs_` under `graphs_lock_`; see 08 for the design)
- F4: `dbms.takeSnapshot()` "Nested transaction" failure under lazy loading —
  **FIXED** (request txn aborted before takeSnapshot, matching
  `dbms.meta.refreshCount()`; see 08)

## Data migration

No data migration is required. The on-disk format is unchanged:
- Graph directories with `data.mdb` are preserved.
- The `_graph_config_table_` in the meta store is unchanged.
- `create_if_not_exist = true` on lazy open means that graphs registered in a
  previous session but never accessed (no `data.mdb`) are created on first
  access in the new session.

## Rollback

Set `lmdb_notls = false` and remove the `max_open_graphs` /
`graph_idle_timeout_s` settings to restore the Phase 0/1 behavior (eager open
of all graphs, no eviction, hard 4096 limit). The on-disk data format is
unchanged, so no data migration is needed for rollback.
