# 07 — Scalability Risks and Components To Change

This is the Phase 0 risk register. Every entry is grounded in code (with
references) and, where possible, in the measured baseline
(`benchmark/scaling/results/BASELINE.md`). Projections to 10k and 100k graphs
are marked as **projections** and are to be confirmed or corrected by
measurement.

## 0. The hard ceiling

### R0 — The real graph limit is ~998, not 4096: one pthread TLS key per LMDB environment

**STATUS: FIXED.** Verified at 2000 and 4000 graphs — see "Resolution" below.
The analysis is retained because it explains why the fix works and why the
obvious alternative (making `MAX_NUM_GRAPHS` configurable) would not have.

This was the most important finding of Phase 0, and it was measured, not
projected.

**Measured behaviour.** Creating graphs in a loop against a running server
(`CALL dbms.graph.createGraph(...)`) fails deterministically at graph **#998**:

```
FIRST FAILURE at graph #998 (total graphs incl default = 998)
  server said: Resource temporarily unavailable
```

The failure index is identical whether the target is 1000 or 4000 graphs, and it
is reproducible across runs. At the moment of failure the process was nowhere
near any kernel resource limit:

| Resource | At failure | Limit | Headroom |
|---|---|---|---|
| Threads (incl. active client) | ~1074 (`cgroup pids.current`) | `pids.max = max` | unlimited |
| File descriptors | 3015 | `ulimit -n` = 1048576 | huge |
| Memory mappings | 4367 | `vm.max_map_count` = 262144 | huge |
| RSS | ~1.9 GiB | container `--memory=6g` | ~4 GiB |
| `ulimit -u` | — | 32137 | huge |
| `threads-max` | — | 64275 | huge |

A control experiment in the same container spawned **3000 additional threads**
successfully, so thread creation was not blocked at the system level either.

**Root cause.** Each graph is a separate LMDB environment. TuGraph opens LMDB
with the flags

```c
// src/core/lmdb_store.cpp:60-65
#if LGRAPH_SHARE_DIR
    unsigned int flags = MDB_NOMEMINIT | MDB_NORDAHEAD | MDB_NOTLS | MDB_NOSYNC;
#else
    unsigned int flags = MDB_NOMEMINIT | MDB_NORDAHEAD | MDB_NOSYNC;
#endif
```

`MDB_NOTLS` is **absent** in the default build. Without it, LMDB allocates a
**pthread thread-specific-data key per environment**:

```c
// src/core/lmdb/mdb.c:5211-5212
if (!(env->me_flags & MDB_NOTLS)) {
    rc = pthread_key_create(&env->me_txkey, mdb_env_reader_dest);
```

glibc caps process-wide TLS keys at `PTHREAD_KEYS_MAX = 1024`, which is a hard
compile-time limit and **not** an adjustable rlimit. Verified directly:

```
$ gcc -o keytest keytest.c -lpthread && ./keytest
pthread_key_create failed after 1024 keys: rc=11 (Resource temporarily unavailable)
$ getconf PTHREAD_KEYS_MAX
1024
```

`rc=11` is `EAGAIN`, whose `strerror` is exactly the `Resource temporarily
unavailable` the server returned.

**Why it fails at 998 rather than 1024.** The process needs 998 graph
environments plus 1 for the Galaxy meta store = 999, and roughly 25 further keys
are consumed by glibc, brpc, cpprestsdk and the Python runtime. Those together
reach the 1024 ceiling, so the 999th graph environment is the one that fails.

**Direct LMDB-level proof.** A standalone program was linked against this
repo's own LMDB (`src/core/lmdb/mdb.c`) and opened environments in a loop using
exactly the flags `LMDBKvStore::Open` uses, with and without `MDB_NOTLS`:

```
=== default flags (exactly what TuGraph uses: NO MDB_NOTLS) ===
TLS   1000 envs opened OK...
TLS   FAILED opening env #1025: rc=11 (Resource temporarily unavailable)

=== with MDB_NOTLS added ===
NOTLS 1000 envs opened OK...
NOTLS 2000 envs opened OK...
NOTLS 3000 envs opened OK...
NOTLS 4000 envs opened OK...
NOTLS 5000 envs opened OK...
NOTLS opened 5000 envs without failure
```

This confirms all three parts of the mechanism: the `EAGAIN` (`rc=11`), the
~1024 ceiling, and that the ceiling disappears entirely once `MDB_NOTLS` is set.
The 1025-vs-998 difference is exactly the ~25 TLS keys the server process has
already consumed for other libraries.

Reproduce the experiment with:

```bash
ci/phase0/experiments/run_lmdb_tls_limit.sh
```

Source: `ci/phase0/experiments/lmdb_tls_limit.c`.

**Consequences for the project.**

- The documented `MAX_NUM_GRAPHS = 4096` (`src/core/defs.h:143`) is
  **unreachable**. The effective limit is ~1000 graphs and is not configurable.
- The Phase 0 requirement to benchmark 1000 and 4000 graphs **cannot be met on
  this build**. The 1000- and 4000-graph runs stop at 998 by construction; their
  results are recorded as measured limits rather than as scaling curves.
- The 10k / 100k graph goals of the wider project are **not reachable by tuning**.
  They require changing how graph storage is organized (fewer LMDB environments,
  or a different storage engine), not more memory, fds or threads.

**Possible directions (not evaluated in Phase 0, listed only to scope later work).**

1. Add `MDB_NOTLS` to the LMDB open flags. **The experiment above shows this
   removes the ceiling outright** (5000 environments opened without failure).
   The problem is that the only way to get `MDB_NOTLS` in this codebase today is
   `ENABLE_SHARE_DIR=ON` (`src/core/lmdb_store.cpp:60-65`), and
   `LGRAPH_SHARE_DIR` also changes HA semantics materially (dummy snapshots,
   reload on leader start — see [04](04-ha-raft-replication.md) §5). The work is
   therefore to decouple "use MDB_NOTLS" from "share-dir mode" and then evaluate
   `MDB_NOTLS` semantics on its own (it changes reader-slot handling, so it must
   be validated as a behavioural change, not a build tweak). This is the
   cheapest path to ~10k graphs and should be assessed first.
2. Share a single LMDB environment across graphs (many logical graphs inside one
   env), removing the one-env-per-graph assumption entirely. This is the change
   that actually addresses 10k+ graphs, and it touches `GraphManager`,
   `LightningGraph`, `LMDBKvStore` and the graph-directory model.
3. Replace the storage engine, or introduce an environment-multiplexing layer
   that opens environments lazily with a bounded pool (LRU eviction of idle
   graphs), which raises the ceiling to the pool size rather than an absolute
   ~1000.

**Phase 0 disposition.** Per the Phase 0 brief, "do not begin optimization work
unless the baseline results demonstrate an issue that prevents completion of this
phase." This issue prevented completion of the 1000/4000-graph measurement
criterion, so it was the justified trigger for the fix that follows.

#### Resolution (implemented and verified)

`LMDBKvStore` now opens environments with `MDB_NOTLS` by default, controlled by
the runtime setting `lmdb_notls` (default **true**):

| File | Change |
|---|---|
| `src/core/global_config.h` | new `BasicConfigs::lmdb_notls` field |
| `src/core/global_config.cpp` | registered with the argparser, the JSON config map, and `ToFieldDataMap()` so it is visible via `dbms.config.list()` |
| `src/core/lmdb_store.h/.cpp` | process-wide `SetUseNotls`/`UseNotls`; `Open()` adds `MDB_NOTLS` when enabled |
| `src/db/galaxy.cpp` | applies the setting in `ReloadFromDisk`, the single place where the meta store and every graph environment are (re)opened — this also covers embedded users and config reloads, not just the server |
| `test/main.cpp` | test-only `--lmdb_notls` switch so the suite can be A/B'd against one binary |

`LGRAPH_SHARE_DIR` still forces `MDB_NOTLS`, unchanged. `DBConfig` was
deliberately not touched: it is binary-serialized into the meta store and read
with an exact length check, so adding a field would have made existing data
directories unreadable.

**Verified result** (same container limits as the Phase 0 baseline,
`--cpus=4 --memory=6g`; full data in `benchmark/scaling/results/R0-VERIFY.md`):

| Graphs | Created | Threads | FDs | Mappings | RSS | Restart | Shutdown |
|---|---|---|---|---|---|---|---|
| 2000 | **1999/1999** | 2,074 | 6,022 | 8,867 | 3.8 GiB | 1.45 s | 1.57 s |
| 4000 | **3999/3999** | 4,074 | 12,022 | 17,014 | 6.2 GiB | 2.92 s | 3.10 s |

4000 graphs — previously impossible — now work, and the Phase 0 criterion
"benchmarks run against 1, 100, 1,000 and 4,000 graphs" is met. Per-graph costs
remain linear: ~1 thread, ~3 fds, ~4.4 mappings, ~1.5 MiB RSS, ~0.73 ms restart
per graph. Integration and failure tests pass (37/37).

**Phase 1 verification went further: 10,000 graphs.** At N=10,000 the harness
measured (`benchmark/scaling/results/PHASE1-10K.md`):

| Phase | Result |
|---|---|
| Create | 9999/9999 in **18.6 s** |
| Resources | RSS **6.64 GiB**, 10,074 threads, 30,022 fds, 41,064 mappings |
| Snapshot | 2.5 s (158 MB, 10,001 files) |
| Restart with 10,000 graphs | **9.5 s** |
| Restore into a fresh dir | verified **10,000** graphs |
| Delete all | 9999/9999 in **18.1 s** |
| Shutdown | 3.2 s |

Per-graph RSS at 10,000 graphs is **0.71 MiB**, well below the 1.8 MiB measured
at 800 — the per-graph cost is not constant, it falls as the count grows
(reclaimed pages and shared structures amortise). Correctness held throughout;
the only degradation was the restore server's startup under heavy swap pressure
(22 min with swap 96% full on a 7.8 GiB host), which Phase 1 explicitly permits.

Integration and failure tests pass (41/41, including the configured-limit
boundary tests).

**Read-path cost:** A/B'd on one binary by toggling only `--lmdb_notls`
(single run per arm, so treat as indicative):

| | read tps | write tps | p50 | p99 |
|---|---|---|---|---|
| on | 713.6 / 710.1 | 2220.4 / 2065.5 | 0.224 / 0.240 ms | 0.283 / 0.295 ms |
| off | 676.7 / 690.4 | 2019.9 / 2017.3 | 0.225 / 0.237 ms | 0.282 / 0.292 ms |

No regression; the `MDB_NOTLS` arm is marginally faster, within run-to-run
noise. Raw data: `benchmark/scaling/results/ab_on.json` / `ab_off.json`.
Single run per arm, so treat the small deltas as indicative rather than exact.

**The new binding limit is memory, not a constant.** 4000 graphs cost 6.2 GiB RSS
under a 6 GiB…6.29 GiB cgroup limit, and 10,000 graphs were verified at
**6.64 GiB** — the per-graph cost falls with scale (~1.5 MiB at 800 graphs down
to ~0.7 MiB at 10,000) as pages are reclaimed and structures amortise. Unlike
`PTHREAD_KEYS_MAX`, memory is raisable with RAM.

**Correctness caveat, tracked separately:** an intermittent SIGSEGV exists in the
upstream unit suite that reproduces with the policy **on and off** at the same
rate (see [08-correctness-findings.md](08-correctness-findings.md) F3). It is
pre-existing and not caused by this change, but it should be triaged before the
unit suite is used as a pass/fail gate for further graph-count work.

#### Implications for Phase 1 ("Remove the 4096 Graph Limit")

Phase 1 as written assumes the graph limit is the hard-coded
`MAX_NUM_GRAPHS = 4096` and that the fix is to make it configurable
(`max_graphs = N`). **That premise was written before R0 was understood, and the
R0 fix changes the picture in Phase 1's favour:**

1. **Before the R0 fix**, making `MAX_NUM_GRAPHS` configurable would have been
   pointless: the system stopped accepting graphs at 998, so the constant was
   never reached and no configuration value could have raised the real limit
   (it was `PTHREAD_KEYS_MAX = 1024` in glibc, not a graph-count limit).
2. **After the R0 fix**, the TLS ceiling is gone and `MAX_NUM_GRAPHS = 4096`
   becomes the genuine first *code* limit, reached between 4000 (measured
   working) and 4096. Phase 1's approach — replacing the constant with a
   configurable `max_graphs` and consolidating validation into one authoritative
   path (`src/db/graph_manager.cpp:99,122`) — is therefore now the correct next
   step.
3. **Phase 1's real constraint is memory, not the constant.** At ~1.5 MiB per
   graph, 10,000 graphs need ~15 GiB RSS, and every graph costs a thread, ~3
   fds and ~4.4 mappings. Phase 1 should state those expectations, and its
   verification matrix (4095/4096/4097/10,000 graphs) should be run on a host
   sized accordingly.
4. **The verification matrix is already largely built.** The Phase 0 harness
   (`benchmark/scaling/run_bench.py`) implements create/open/list/restart/
   backup/restore/delete across arbitrary graph counts and is driven by
   `--graphs`, so Phase 1 can reuse it directly by changing one flag.
5. **Two pre-existing correctness defects were fixed during Phase 2
   validation** — F1 (`UNWIND ... CREATE` under-inserts under the default
   Cypher v2 engine) and F2 (label-filtered `count()` fails on an empty label)
   — see [08-correctness-findings.md](08-correctness-findings.md). They no
   longer block clean graph-count work. F3 (the intermittent unit-suite
   SIGSEGV) remains open and still prevents using the unit suite as a clean
   gate; the Phase 2-introduced eviction crash F3a has been fixed
   (eviction task now owned by `Galaxy`).

Everything else in Phase 1 — finding every graph-count limit, consolidating
validation into one authoritative path, verifying no storage/ID/serialization
format assumes 4096 — remains valid work and is supported by the inventory in
[01](01-graph-lifecycle.md) and this document.

---

## 1. Risk register

Severity is the expected impact on a 10k+ graph deployment. R0 above is the
BLOCKER; R1-R14 are the structural risks that remain relevant once R0 is solved.

### R1 — One OS thread per graph (severity: critical)

- **Evidence:** `LMDBKvStore`'s constructor unconditionally starts a validator
  thread, for every graph and for the meta store
  (`src/core/lmdb_store.cpp:105`). Each poll interval is 100 ms
  (`src/core/lmdb_store.cpp:231-237`), so nothing about the workload reduces the
  count.
- **Cost:** N + 1 threads; ~8 MiB of virtual address space per stack.
  - 4,000 graphs → ~4,001 threads, ~32 GiB of stack VA.
  - 10,000 → ~10,001 threads, ~80 GiB VA. (projection)
  - 100,000 → ~100,001 threads, ~800 GiB VA. (projection)
- **Failure mode:** thread creation fails (`EAGAIN`/`ENOMEM`) or the container's
  pids cgroup limit is hit long before the graph cap. This is likely to be the
  first hard wall, well before 4096 in a constrained container.
- **Required change:** a shared, bounded validator pool (or event-driven
  validation) instead of one thread per store. The validator only serves
  optimistic transactions (`src/core/lmdb_store.cpp:231-372`), so a pool is
  viable.

### R2 — Sequential eager open of every graph at startup (severity: critical) — **FIXED by Phase 2 lazy loading**

- **Evidence:** `GraphManager::ReloadFromDisk` is a single-threaded loop that
  constructs a `LightningGraph` per config row
  (`src/db/graph_manager.cpp:257-284`). The server does not log
  `"Server started."` until it finishes (`src/server/lgraph_server.cpp:313,379`).
- **Cost:** O(N) × per-graph open cost. Per-graph open includes an LMDB env
  creation, six-plus table opens, a `PluginManager`, and — if any vector index
  exists — a **full row scan** to rebuild it in memory
  (`src/core/index_manager.cpp:97-147`).
- **Failure mode:** startup time grows linearly and uninterpretably; with vector
  indexes it can grow with total data size. At 10k+ graphs the process may take
  longer to start than an operational window allows.
- **Required change:** lazy/on-demand graph open with an LRU of open graphs, and
  parallel open. This implies changing what `GraphManager` owns — currently it
  holds a live `LightningGraph` for every graph.

### R3 — Graph lifecycle operations are O(N) under a global exclusive lock (severity: critical) — **BOUNDED by Phase 2**

- **Evidence:** `Galaxy::CreateGraph` and `DeleteGraph` copy-construct the entire
  `GraphManager` **and** `AclManager` while holding `acl_lock_` and `graphs_lock_`
  write locks (`src/db/galaxy.cpp:192-193`, `:217-218`).
- **Cost:** O(N) `GraphManager` copy (N string keys + N refcount increments) plus
  O(users × graphs) `AclManager` copy, per operation. Creating N graphs
  sequentially is **O(N²)**, and every operation blocks all graph opens and all
  listings.
- **Failure mode:** with a non-trivial ACL, bulk graph provisioning degrades
  quadratically and stalls the whole server during each operation.
- **Required change:** stop copy-on-write of the whole registries. Use
  fine-grained locking, a persistent/immutable registry, or sharded registries.
  This is arguably the single most impactful change for graph-count scaling.

### R4 — Per-graph virtual address reservation (severity: high) — **BOUNDED by Phase 2 lazy loading**

- **Evidence:** `db_size` defaults to 4 TiB (`DEFAULT_GRAPH_SIZE = 1<<42`,
  `src/core/defs.h:154`) and is applied when a caller passes 0
  (`src/db/graph_manager.cpp:103-104`). LMDB `mmap`s the whole map size
  (`src/core/lmdb_store.cpp:56`). The auto-created `default` graph hits this path
  (`src/db/graph_manager.cpp:242-255`).
- **Cost:** `db_size` of virtual address space per graph, plus memory mappings.
  - At 1 GiB/graph: 4,000 graphs → 4 TiB VA — comfortable on a 47-bit host.
  - At the 4 TiB default: ~30 graphs exhaust a 128 TiB user VA. (projection)
- **Failure mode:** `mdb_env_open` fails with `ENOMEM`, or
  `vm.max_map_count` is exceeded. REST creation cannot hit this (it requires an
  explicit `max_size_GB`, `src/restful/server/json_convert.h:536-548`), but
  embedded/Cypher callers that omit the size can.
- **Required change:** a sane default `db_size`, and a documented policy that
  graph count drives `db_size` downward.

### R5 — ~60 KiB of reference-count arrays per graph (severity: medium-high)

- **Evidence:** `RefCountedObj::references_` is a vector of
  `LGRAPH_MAX_THREADS = 480` (`src/core/thread_id.h:21`) entries of
  `PadForCacheLine<uint64_t>` = 64 bytes
  (`include/fma-common/type_traits.h:178-186`, `src/core/managed_object.h:55-60`).
  Each graph has at least two such objects: `GcDb` (the `LightningGraph`) and
  `LightningGraph::schema_` (`src/core/lightning_graph.h:58`).
- **Cost:** ~30 KiB each ⇒ **~60 KiB per graph** independent of graph size.
  - 4,000 graphs → ~240 MiB.
  - 10,000 → ~600 MiB. 100,000 → ~6 GiB. (projections)
- **Secondary cost:** "has any reference?" requires scanning all 480 slots
  (`src/core/managed_object.h:71-76`), and destruction scans them.
- **Note:** `PadForCacheLine` pads to 64 bytes but has **no `alignas`**, so these
  entries are not actually cache-line aligned despite the name — a latent
  performance bug worth flagging separately.
- **Required change:** size the reference array from the actual thread budget, or
  replace the per-object TLS array with a scalable refcount scheme.

### R6 — Single shared meta store for the graph registry and ACL (severity: high)

- **Evidence:** one LMDB env at `<db_dir>/.meta` with a fixed 1 GiB map holds
  `_graph_config_table_`, `_user_table_`, `_role_table_`, `_ip_whitelist_`,
  `_meta_` (`src/db/galaxy.cpp:554-572`). Every graph lifecycle and ACL mutation
  writes through it.
- **Cost:** O(1) per write in principle, but a single LMDB env has a single
  writer lock, so all graph registration serializes globally. ACL memory is
  O(users × graphs) (`src/db/acl.h:139-203`, `src/db/acl.cpp:76-107`).
- **Failure mode:** metadata write contention and ACL memory growth at 10k+
  graphs; the fixed 1 GiB map is also a hard size ceiling for registry entries.
- **Required change:** partition/shard the graph registry, and decouple ACL
  graph-access maps from a per-role dense structure.

### R7 — Single global Raft group; partial replication coverage (severity: critical for HA)

- **Evidence:** one `RawNode` per process (`src/bolt_raft/raft_driver.h:143`,
  `src/server/bolt_raft_server.cpp:54-56`); one braft node with group id
  `"lgraph"` (`src/server/ha_state_machine.cpp:80-81`). No per-graph
  attach/detach anywhere in `src/bolt_raft/`.
- **Sub-risks:**
  - All writes across all graphs share one global consensus order — it cannot
    scale with graph count.
  - Bolt HA replicates **only** the Bolt `Run` path
    (`src/server/bolt_handler.cpp:282-299`); REST/RPC/admin writes are not
    replicated by it, silently risking replica divergence.
  - Bolt HA previously returned `ErrSnapshotTemporarilyUnavailable` unconditionally,
    so a follower behind the compacted prefix could never catch up. The store now
    keeps the etcd-raft snapshot contract (`SetSnapshot`/`GetSnapshotMeta`/
    `ApplySnapshot`) and log GC is bounded by the slowest peer's acknowledged
    index, so a lagging follower recovers by log replay; only a brand-new peer
    on a fully-compacted log still needs a data-carrying snapshot.
  - Legacy braft applies whole requests and interacts with O(N) stop-the-world
    reload/snapshot paths.
- **Required change:** decide the replication unit (per-graph vs sharded
  consensus groups), and make replication coverage uniform across all write
  surfaces. This is the central architectural obstacle to a horizontally
  scalable, highly available multi-graph platform.

### R8 — Backup/snapshot/restore are whole-server, serial and blocking (severity: high)

- **Evidence:** `GraphManager::Backup` is a serial loop over all graphs
  (`src/db/graph_manager.cpp:298-316`); `Galaxy::Backup` holds the `reload_lock_`
  **write** lock (`src/db/galaxy.cpp:591`); `Galaxy::SaveSnapshot` holds it in
  **read** mode despite the code's own TODO saying write is required
  (`src/db/galaxy.cpp:506-509`); `Galaxy::LoadSnapshot` deletes every graph dir
  then reopens everything (`src/db/galaxy.cpp:481-496`).
- **Cost:** O(N) serial LMDB copies, no per-graph granularity, no parallelism, no
  incremental *data* backup (the binlog is request-level and misses the Bolt
  path).
- **Failure mode:** at 10k graphs a snapshot/backup is a long all-or-nothing
  operation; a full backup is a complete service outage.
- **Required change:** per-graph or sharded backup, parallel copies, and
  MVCC-based online snapshots. The code already identifies MVCC as the intended
  solution (`src/db/galaxy.cpp:506-508`).

### R9 — Global locks and a globally-serialized allocator on the request path (severity: high)

- **Evidence:** every RPC/REST request takes `Galaxy::reload_lock_` read
  (`src/server/state_machine.cpp:251`) and at least one `acl_lock_` read for
  token validation (`src/db/galaxy.cpp:125`). Every Cypher/GQL allocation takes a
  process-wide mutex in `AllocatorManager`
  (`src/cypher/monitor/memory_monitor_allocator.h:41,49`;
  `src/cypher/monitor/monitor_manager.h:48,56`).
- **Cost:** fixed per-request lock traffic that does not shard with graph count;
  the allocator mutex scales with allocation count.
- **Failure mode:** lock contention becomes the throughput ceiling well before
  CPU saturation, especially with many graphs and many small queries.
- **Required change:** shard or remove the global locks; make memory accounting
  per-thread/per-user with periodic aggregation instead of per-allocation.

### R10 — One OS thread per Bolt connection (severity: medium-high)

- **Evidence:** `Hello` spawns a detached `std::thread(BoltFSM)` per authenticated
  connection (`src/server/bolt_handler.cpp:380-381`), and `LGRAPH_MAX_THREADS`
  caps thread-id slots at 480 (`src/core/thread_id.h:21`).
- **Cost:** threads = graphs + Bolt connections. These compete for the same
  thread budget as R1.
- **Required change:** bounded worker pool with async session state, replacing
  thread-per-connection.

### R11 — Bolt bypasses the state machine (severity: medium, correctness)

- **Evidence:** `BoltFSM` calls `Scheduler::Eval` directly
  (`src/server/bolt_handler.cpp:302`), so it takes neither `reload_lock_` nor the
  binlog write path taken by `StateMachine::HandleRequest`
  (`src/server/state_machine.cpp:150-163,251`).
- **Consequences:** Bolt queries can race a concurrent `ReloadFromDisk`; Bolt
  writes are absent from the binlog and from legacy braft replication.
- **Required change:** unify the request path, or make the mutual exclusion
  explicit and documented.

### R12 — `thread_local` plan cache, not shared (severity: medium)

- **Evidence:** Cypher v1/v2 plan caches are `thread_local` LRUs keyed only by
  query string, capacity 256 (`src/cypher/execution_plan/scheduler.cpp:74,170`;
  `lru_cache.h:95`). GQL has no cache.
- **Cost:** every Bolt connection thread rebuilds plans; memory scales with
  threads × cache. Whether cached plans carry graph-specific validation state is
  unverified — a potential correctness issue for multi-graph workloads.
- **Required change:** a shared, bounded plan cache keyed by (query, schema
  version), and verification that cached plans are graph-independent.

### R13 — Environment/capacity limits are undocumented in-product (severity: high)

- **Evidence:** the process needs `nofile ≥ 2 × graphs`, thread headroom, and
  `vm.max_map_count` headroom, but nothing in the server validates or logs these
  against `MAX_NUM_GRAPHS`. A default `nofile` of 1024 fails at ~500 graphs.
- **Measured instance (see R0):** the real graph ceiling is a `pthread_key_create`
  EAGAIN at ~998 graphs. It surfaces to the client only as an HTTP 500
  `Resource temporarily unavailable` with no indication that a TLS-key limit was
  reached, no mention of graph count, and no server-side warning. An operator
  cannot diagnose this without reading LMDB and glibc internals.
- **Required change:** a startup capacity check that compares planned graph count
  against the real resources (including the TLS-key budget) and fails fast with an
  actionable message; and a clear, typed error when graph creation is rejected
  because a process-level limit was reached.

### R14 — Build is not reproducible from this checkout, and is x86-assuming (severity: medium, process)

- **Evidence (provenance):** the pinned compile image was built from a newer
  revision of `ci/images/tugraph-compile-arm64v8-centos7-Dockerfile` than exists
  in this checkout — the image's history references `ci/images/vendor/`, which
  is absent here. `docker build` from this checkout does **not** reproduce the
  pinned image. See `ci/phase0/env/README.md`.
- **Evidence (x86 assumption):** `deps/geax-front-end/CMakeLists.txt:52-61`
  applies `-msse4.2`, an x86-only flag, unless `ENABLE_BUILD_ON_AARCH64=ON`.
  That option exists upstream but defaults to OFF and is not mentioned in any
  build instruction, so a clean arm64 build fails with
  `c++: error: unrecognized command line option '-msse4.2'`.
- **Evidence (toolchain pin drift):** `src/cython/lgraph_db_python.py` uses
  Cython 3.0 pure-Python-mode syntax, but the pinned image ships Cython 0.29.37,
  so the `lgraph_db_python` target fails with
  `Cannot take address of Python object attribute 'db'`. The repo's own
  Dockerfile pins `cython==3.0.0a11`, an alpha that was never published to PyPI.
- **Phase 0 mitigation:** `ci/phase0/build.sh` auto-detects aarch64 and passes
  `-DENABLE_BUILD_ON_AARCH64=ON`; `ci/phase0/env/Dockerfile.phase0` builds a
  derived image adding Cython 3.0.0, pinned by its own image ID in
  `ci/phase0/images.lock`. `ci/phase0/doctor.sh` verifies the image ID and the
  presence of every required library on every run, so a divergent environment
  fails fast instead of producing incomparable numbers.
- **Required change:** rebuild the toolchain image from source, diff it against
  the pinned ID, and either fix or document `-msse4.2` and the Cython pin
  upstream so that a clean ARM build needs no out-of-band knowledge.

## 2. Bottleneck projections

Per graph, the fixed costs are: 1 thread, ~3 fds, ~4.4 mappings, `db_size` VA,
~60 KiB refcount arrays, and O(N) participation in every lifecycle op and list
call. **These are now measured up to 4000 graphs**, no longer projected:

| Resource | 4,000 graphs (measured) | 10,000 (proj.) | 100,000 (proj.) |
|---|---|---|---|
| Threads | 4,074 | ~10,001 | ~100,001 |
| File descriptors | 12,022 | ~30,000 | ~300,000 |
| Memory mappings | 17,014 | ~43,000 | ~430,000 |
| RSS | 6.2 GiB | ~15 GiB | ~150 GiB |
| VA at 1 GiB/graph | 8.6 TiB | 10 TiB | 100 TiB |
| Refcount arrays | ~240 MiB | ~600 MiB | ~6 GiB |
| Startup | 0.13 s | O(N) | O(N) |
| Restart w/ N graphs | 2.92 s | ~7.3 s | ~73 s |
| Shutdown | 3.10 s | ~7.7 s | ~77 s |
| Create N graphs | O(N²) | O(N²) | O(N²) — impractical |
| List graphs | O(N) per call | O(N) | O(N) |
| Snapshot/backup | O(N) serial, blocking | O(N) | O(N) — impractical |
| Raft | constant, 1 global group | constant but globally serialized | same |

**The binding constraints after the R0 fix, in order, are:** memory (~1.5 MiB per
graph — 10k graphs needs ~15 GiB), then `MAX_NUM_GRAPHS = 4096` (the first *code*
limit), then R2 (startup/restart is linear but cheap: 2.9 s at 4000 graphs), then
R3 (O(N²) bulk creation). The earlier expectation that threads would be the first
wall was wrong: 4,074 threads work fine, though it is a large and unusual thread
count for a single process and worth watching in production.

## 3. Components likely to require modification in later phases

Ordered by expected necessity, with the specific code to change.

| Priority | Component | File(s) | Why |
|---|---|---|---|
| 0 | One LMDB environment per graph / `MDB_NOTLS` flag | `src/core/lmdb_store.cpp:60-75`, `src/db/galaxy.cpp:551-560` | **FIXED** — `MDB_NOTLS` on by default via `lmdb_notls`; ceiling removed, verified at 4000 graphs |
| 1 | `MAX_NUM_GRAPHS` and graph-count validation | `src/core/defs.h` (`CheckValidGraphNum`), `src/db/graph_manager.cpp:99,122` | **DONE** — replaced by configurable `max_graphs` (0 = unlimited); off-by-one between the two creation paths fixed |
| 1 | `GraphManager` ownership model | `src/db/graph_manager.h:36-67`, `src/db/graph_manager.cpp:233-296` | eager open of all graphs; no internal locking; O(N) copy |
| 1 | `Galaxy::CreateGraph`/`DeleteGraph`/`ModGraph` COW | `src/db/galaxy.cpp:184-260` | O(N) registry + O(users × graphs) ACL copy under a global lock |
| 1 | `LMDBKvStore` validator thread | `src/core/lmdb_store.cpp:105,231-372` | 1 thread per graph |
| 2 | `AclManager` graph-access representation | `src/db/acl.h:139-203`, `src/db/acl.cpp:76-107` | dense per-role/per-user graph maps |
| 2 | Graph registry storage | `src/db/galaxy.cpp:554-572`, `src/db/graph_manager.cpp:50-56` | one shared 1 GiB meta env, single writer |
| 2 | Backup/snapshot/restore | `src/db/galaxy.cpp:481-531,590-604`, `src/db/graph_manager.cpp:298-316` | whole-server O(N), blocking, serial |
| 2 | Raft replication unit | `src/bolt_raft/*`, `src/server/ha_state_machine.cpp` | one global group; partial coverage; snapshot metadata kept, GC bounded by slowest peer |
| 3 | `RefCountedObj` reference array | `src/core/managed_object.h:55-60`, `src/core/thread_id.h:21` | ~60 KiB/graph, O(480) scans |
| 3 | Global locks | `src/db/galaxy.h:62,71,73` | one RW lock pair for all graphs |
| 3 | `AllocatorManager` | `src/cypher/monitor/monitor_allocator*`, `monitor_manager.h:48` | per-allocation global mutex |
| 3 | Bolt threading and request path | `src/server/bolt_handler.cpp:302,380` | thread-per-connection; bypasses state machine |
| 4 | Plan cache | `src/cypher/execution_plan/scheduler.cpp:74,170` | thread-local, not graph-keyed |
| 4 | Capacity validation at startup | `src/server/lgraph_server.cpp:116-386` | open-graph cache estimate logged with a 4 GiB warning; no fd/mmap budget checks |
| 4 | Vector index open cost | `src/core/index_manager.cpp:97-147` | full scan per open |

## 4. What Phase 0 explicitly does not do

No optimization, no architectural change, no sharding, no per-graph consensus.
Phase 0 establishes the baseline and the risk register. Optimization begins only
once the baseline demonstrates something that prevents completing Phase 0 — and
per the Phase 0 success criteria, the baseline itself is the deliverable.
