# TuGraph Architecture Baseline (Phase 0)

This directory documents the TuGraph 4.5.2 architecture as it actually exists at
commit `672e4b199`, with a focus on everything that determines **how the system
behaves as the number of graphs grows**. It is the reference for the Phase 0
baseline and for planning later scaling phases.

Scope discipline: these documents describe the code, not a desired design. Where
something is a scaling limit, it is called out explicitly as such.

## Documents

| # | Document | Covers |
|---|---|---|
| — | [README.md](README.md) | This index, plus the system in one page |
| 01 | [01-graph-lifecycle.md](01-graph-lifecycle.md) | Galaxy, GraphManager, LightningGraph; graph create/delete/open/close and the resources each graph consumes |
| 02 | [02-storage-and-transactions.md](02-storage-and-transactions.md) | LMDB layout, per-graph on-disk and in-memory footprint, MVCC, transaction lifecycle, indexes, schema |
| 03 | [03-metadata-and-acl.md](03-metadata-and-acl.md) | Meta store, graph config table, users/roles/permissions, token handling |
| 04 | [04-ha-raft-replication.md](04-ha-raft-replication.md) | The two independent Raft subsystems, their replication unit, storage and recovery |
| 05 | [05-backup-restore-snapshot.md](05-backup-restore-snapshot.md) | Snapshot, backup, binlog and restore paths and their blocking behaviour |
| 06 | [06-server-and-request-paths.md](06-server-and-request-paths.md) | Server startup order, thread pools, global locks, Bolt/REST/OpenCypher request paths |
| 07 | [07-scalability-risks.md](07-scalability-risks.md) | Bottlenecks at 10k/100k graphs, risk register, components likely to need modification |
| 08 | [08-correctness-findings.md](08-correctness-findings.md) | Correctness defects found while building the Phase 0 harness (not fixed in Phase 0) |
| 09 | [09-test-baseline.md](09-test-baseline.md) | Build, upstream unit-test and integration-test baseline, with the pass/fail record |
| 10 | [10-graph-lifecycle-v2.md](10-graph-lifecycle-v2.md) | Phase 2 migration notes: lazy loading, eviction, new config knobs, behaviour changes |

> **Two correctness defects found during Phase 0 require attention before any
> scaling work:** `UNWIND ... CREATE` under-inserts in the default Cypher v2
> engine, and a label-filtered `count()` fails on an empty label. See
> [08-correctness-findings.md](08-correctness-findings.md).

## The system in one page

TuGraph is a **single-process, embedded-LMDB graph database with an optional
multi-graph front end and optional Raft-based HA**. There is one process, one
`Galaxy`, one `GraphManager`, and N `LightningGraph` objects — one per named
graph. Every graph is a completely separate LMDB environment with its own
directory, its own tables, and its own background thread.

```
lgraph_server (one process)
│
├── GlobalConfig / argparser
├── StateMachine  (or HaStateMachine)
│   ├── cypher::Scheduler            — per-process query scheduler
│   └── Galaxy                       — the whole database instance
│       ├── KvStore store_           — ".meta" LMDB env (1 GiB map)
│       │   └── tables: _meta_, _graph_config_table_,
│       │               _ip_whitelist_, _user_table_, _role_table_
│       ├── AclManager acl_          — global (server-wide) users/roles
│       ├── TokenManager             — in-memory JWT bindings
│       └── GraphManager graphs_
│           └── unordered_map<name, GCRefCountedPtr<LightningGraph>>
│               ├── "default"    → <db_dir>/<hex1>/   (LMDB env + thread)
│               ├── "<graph_2>"  → <db_dir>/<hex2>/   (LMDB env + thread)
│               └── ...                                (1 thread each)
│
├── brpc RPC server (optional)
├── cpprestsdk REST server
├── Bolt server (optional)
└── BoltRaftServer (optional) — ONE global Raft group
```

### The five facts that dominate scaling

1. **Every graph is opened eagerly at startup, sequentially.**
   `GraphManager::ReloadFromDisk` iterates the graph config table and constructs
   a `LightningGraph` for each row (`src/db/graph_manager.cpp:257-284`). Server
   start blocks until all N graphs are open. Startup time is therefore O(N).

2. **Every graph costs one background thread and file descriptors.**
   `LMDBKvStore`'s constructor unconditionally starts a validator thread
   (`src/core/lmdb_store.cpp:105`) that polls every 100 ms
   (`src/core/lmdb_store.cpp:231-237`). Each open LMDB env holds `data.mdb` and
   `lock.mdb`; measured across 1 → 998 graphs the process grows by ~1 thread,
   ~3 fds and ~4.4 memory mappings per graph. At 998 graphs that is ~1,074
   threads and ~3,015 fds.

3. **Every graph reserves a huge virtual address range.**
   The default `db_size` is 4 TiB (`src/core/defs.h:154`) and LMDB `mmap`s the
   whole map size. This is virtual address space, not resident memory, but it
   still bounds how many graphs can be open at once and consumes mappings.

4. **Graph lifecycle operations are O(N).**
   `Galaxy::CreateGraph`, `DeleteGraph` and `ModGraph` copy-construct the entire
   `GraphManager` **and** the entire `AclManager` while holding the global
   `graphs_lock_` write lock (`src/db/galaxy.cpp:192-193`, `:217-218`). Creating
   the Nth graph copies N-1 existing graph pointers and blocks all graph opens
   for the duration.

5. **HA/Raft does not scale with graphs — it does not see them at all.**
   Both Raft subsystems use a single process-wide group and one node. Graph
   identity is just a string inside a replicated request or Cypher statement.

### Where the hard limits are

| Limit | Value | Where |
|---|---|---|
| **Max graphs openable** | **10,000 measured working** (configurable via `max_graphs`) | `CheckValidGraphNum` (`src/core/defs.h`); the former hard-coded 4096 and the ~998 TLS-key ceiling are both gone — see [07](07-scalability-risks.md) R0 |
| Max graphs (documented, unreachable) | 4096 | `src/core/defs.h:143`, enforced `src/db/graph_manager.cpp:99` |
| Max labels per graph | 4096 | `src/core/defs.h:145` |
| Max fields per label | 1024 | `src/core/defs.h:144` |
| Max tables (DBIs) per graph | 10000 | `src/core/lmdb_store.cpp:58` |
| Max readers per graph env | 1200 | `src/core/lmdb_store.cpp:59` |
| Default/ max graph mmap | 4 TiB / 16 TiB | `src/core/defs.h:154-155` |
| Meta store mmap | 1 GiB | `src/db/galaxy.cpp:554` |
| Max users | 65536 | `src/core/defs.h:142` |

> **The former ceiling of ~1000 graphs was removed.** Each graph is its own LMDB
> environment, and the default build originally did not pass `MDB_NOTLS`, so
> LMDB allocated one pthread TLS key per graph and hit glibc's
> `PTHREAD_KEYS_MAX = 1024` at graph #998. The engine now sets `MDB_NOTLS` by
> default (configurable via `lmdb_notls`), and 4000 graphs were measured working.
> See [07-scalability-risks.md](07-scalability-risks.md) R0 for the analysis and
> the fix's verification.

The binding constraint is now **memory**: 10,000 graphs cost 6.6 GiB RSS
(~0.7 MiB per graph — the per-graph cost falls with scale). `max_graphs` is
configurable (0 = unlimited), so resource limits are the natural bound.
