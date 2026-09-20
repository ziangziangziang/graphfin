# 13 — Storage-Engine Memory Footprint and Levers

Compute/RAM are expensive for this team, so this reviews where a running
`lgraph_server` spends memory and which parts can be turned off while testing
sharding. Numbers are grounded in the code; measured baselines are in
[07-scalability-risks.md](07-scalability-risks.md).

## 1. The two populations of graphs

The Phase 2 refactor is the key to the whole analysis:

- **Registered** graphs (up to 100k+) cost only the catalog entry:
  `GraphCatalog` holds one `unordered_map<string, DBConfig>` entry per graph
  (~150–200 B, mostly the name/desc/dir strings). No storage, no thread, no
  storage engine.
- **Open** graphs cost the expensive per-graph resources, and the open set is
  bounded by `max_open_graphs` (LRU-evicted, Phase 2).

So the memory that grows with the *registered* count is tiny; the memory that
scales is `max_open_graphs × per-open-graph cost`.

## 2. Per-open-graph cost breakdown

| Component | Cost / open graph | Source | Scales with |
|---|---|---|---|
| Refcount arrays | **~60 KiB** | `RefCountedObj::references_` = `LGRAPH_MAX_THREADS(480) × 64 B`, ~2 per graph (`src/core/managed_object.h`, `src/core/thread_id.h:21`) | `max_open_graphs` |
| LMDB env structures | ~540 KiB *virtual*, ~50 KiB resident | `calloc(maxdbs, …)` for `me_dbxs/dbflags/dbiseqs` (`src/core/lmdb/mdb.c:5506-5508`); lazily faulted | `max_open_graphs` |
| LVLMDB map (`db_size`) | 4 TiB *virtual* (default) | `mdb_env_set_mapsize` (`src/core/lmdb_store.cpp:66`) | `max_open_graphs` |
| Validator thread | 1 thread; stack mostly virtual | `lmdb_store.cpp:119` | `max_open_graphs` |
| LMDB lock/reader table | ~29 KiB mmap per env | `mdb_env_set_maxreaders(env, 1200)` (`lmdb_store.cpp:68`) | `max_open_graphs` |
| Page cache | grows with data touched | OS, reclaimable | data size |

Fixed per-process costs (independent of graphs): the RPC/HTTP servers and thread
pools, the two Raft subsystems if enabled, the Prometheus exposer, the Cypher
scheduler/allocator, and the audit logger.

## 3. What does **not** cost memory (checked)

- **Web console**: static assets are **streamed from disk on request**, not held
  in RAM (`rest_server.cpp:958-984`). The only resident part is a
  path→(file, content-type) map — a few hundred bytes per file. Disabling the
  web service saves almost nothing; it is not a useful lever. (`html_content_map_`
  is also empty when the resource dir is not built, as on the test VM.)
- **`lmdb_max_dbs`**: its `calloc` is lazily faulted, so raising it from 1024 to
  10000 changes resident memory by only ~50 KiB/graph (see the note in
  `lmdb_store.h`). It is a *virtual/reservation* lever, not a big RSS one.

## 4. Levers, ordered by impact and risk

### Configuration (no rebuild, immediate)

| Lever | Effect | Notes |
|---|---|---|
| `--max_open_graphs N` | **Primary.** Caps open envs → caps refcount + env + fd + thread cost | Set as low as the working set allows (e.g. 32–128 for sharding tests) |
| `--graph_idle_timeout_s` | Evict idle graphs sooner | e.g. 30 |
| `--lmdb_max_dbs N` | Smaller per-env reservation/VA | Keep ≥ the tables a graph uses (~8 base + labels + indexes); 128–256 is ample for tests |
| `--enable_plugin false` | No plugin/CPP/Python catalogs, fewer DBIs | Default is already false |
| `--monitor_host ""` | No Prometheus exposer/registry | Default is empty |
| `--bolt_port 0 --bolt_raft_port 0` | No Bolt server / Bolt Raft group | Not needed for RPC-only shards |
| `--thread_limit N` | Bounds HTTP thread pool | Fixed process cost, not per graph |
| absent/empty `--web` dir | (Already the case on the VM) | Negligible effect (§3) |

### Build-time

| Lever | Effect | Risk |
|---|---|---|
| `-DLGRAPH_COMPACT_REFCOUNT=1` | Refcount slots become 8 B instead of 64 B → **~60 KiB → ~7.5 KiB per open graph (8x)** | Low: correctness unchanged (each thread writes only its own index); trades false-sharing performance. Intended for memory-constrained, many-graph deployments. |

### Larger (future) work

- `R5`: size the reference array from the actual thread budget, or replace the
  per-object TLS array with a scalable refcount, so the cost is not a fixed
  480 slots. Also fixes the latent `PadForCacheLine` non-alignment.
- `R6`: decouple ACL graph-access maps (O(users × graphs)) from a per-role dense
  structure.
- Make `mdb_env_set_maxreaders(1200)` configurable (`lmdb_max_readers`).

## 5. Recommended sharding-test profile

For the many-node, many-graph sharding tests on small hosts:

```
--max_open_graphs 64
--graph_idle_timeout_s 30
--lmdb_max_dbs 256
--enable_plugin false
--monitor_host ""
--bolt_port 0
--bolt_raft_port 0
```

and build with `-DLGRAPH_COMPACT_REFCOUNT=1`.

At `max_open_graphs = 64` this bounds the open-graph cache to roughly
`64 × (7.5 KiB refcount + ~50 KiB resident env) ≈ 3.7 MiB`, versus ~64 × (60
KiB + 50 KiB) ≈ 6.9 MiB without compact refcounts — and, more importantly,
decouples it from the 100k registered graphs entirely.

## 6. Observability

`lgraph_server` now logs an **open-graph cache estimate** at startup
(`lgraph_server.cpp`, right after the config dump) computed from
`max_open_graphs`, `lmdb_max_dbs`, and the refcount mode, and warns when the
estimate exceeds 4 GiB with pointers to the relevant knobs.

## 7. Document history

| Date | Author | Change |
|---|---|---|
| 2026-09-20 | Phase 4 memory review | Initial footprint review and levers |
