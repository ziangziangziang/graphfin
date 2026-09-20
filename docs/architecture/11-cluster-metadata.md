# 11 — Cluster Metadata (Phase 4A)

## 1. Purpose

Phase 4 introduces whole-graph horizontal sharding. A graph is the atomic
placement unit and belongs to exactly one shard; a shard is itself a Phase 3
replica group. Before any routing can happen, the cluster needs a durable,
consistent **control-plane catalog** answering two questions cheaply:

1. Which shard hosts graph `X`?
2. What shards exist, and are they healthy?

Phase 4A delivers that catalog. It does **not** yet route requests or place
graphs — those are 4B (shard registry + registration) and 4C (router).

## 2. Data model

| Type | Meaning | Size |
|---|---|---|
| `GraphId` | dense, process-local index `0..N-1` | 4 B |
| `ShardId` | shard identity (≤ 65535 shards) | 2 B |
| `PlacementVersion` | monotonic per-graph version; guards stale routing | 8 B |
| `ConfigVersion` | monotonic cluster-wide version | 8 B |
| `GraphPlacement` | shard + state + versions for one graph | **16 B** |
| `ShardInfo` | shard descriptor (name, endpoints, state, weight) | few |

`GraphPlacement` is deliberately a 16-byte, trivially-copyable POD:

```cpp
struct GraphPlacement {
    uint64_t placement_version;  // +0
    uint32_t shard_id;           // +8
    uint16_t generation;         // +12 (number of moves)
    uint8_t  state;              // +14 (PlacementState)
    uint8_t  reserved;           // +15
};
```

`PlacementState`: `CREATING → ACTIVE → MOVING → DELETING → DELETED`.
`ShardState`: `OFFLINE / ONLINE / DRAINING / REMOVED`.

## 3. Memory design (RAM is the scarce resource)

Compute/RAM are expensive for the test fleet, so the catalog is built to scale
to 100k+ graphs with a small, predictable resident cost.

| Structure | Representation | 100k graphs |
|---|---|---|
| Placements | one flat `std::vector<GraphPlacement>` indexed by `GraphId` | 1.6 MiB |
| Names | one byte arena, length-prefixed entries | ~2 MiB |
| Name→id | open-addressing table, 4-byte slots (`id+1`, 0=empty) | ~1 MiB |
| Name offsets | `std::vector<uint32_t>` (id → arena offset) | 0.4 MiB |
| Shards | tiny `unordered_map` | negligible |
| **Total** | | **~5 MiB** |

Contrast with a naive `unordered_map<std::string, GraphPlacementRecord>` plus
per-entry node allocations, which costs well over 100 B/graph (tens of MiB at
100k). The unit test `MemoryFootprintIsCompact` asserts the index stays below
100 B/graph for 20k graphs and logs the actual figure.

Design rules that keep it small:

- **No per-graph `std::string`** — names are interned in one arena; only
  `GraphId`s are hashed and stored.
- **Open addressing, not chaining** — no per-entry node allocation.
- **Dense ids** — placement is an array index, not a map lookup.
- **LMDB is authoritative**, the in-memory index is a rebuildable derived view.
- Deleted graphs are tombstoned in memory (ids stay stable within a run) and
  removed from LMDB; a `live_count_` tracks the real size in O(1).

## 4. Persistence schema

Three LMDB tables inside the existing meta store (the same 1 GiB `.meta` env
Galaxy already uses; Phase 4A reuses it rather than adding a new environment):

| Table | Key | Value |
|---|---|---|
| `_cluster_shard_` | big-endian `uint16` shard id | serialized `ShardInfo` |
| `_cluster_graph_` | graph name (string) | serialized `GraphPlacement` (16 B) |
| `_cluster_meta_` | `"v"` | cluster `ConfigVersion` (8 B) |

Big-endian shard ids make bytewise (default comparator) ordering numeric.
Reload assigns `GraphId`s in iteration order, so ids are dense after restart;
deleted graphs were removed from LMDB, so they do not reappear.

> Control-plane consensus: Phase 4A reuses the local meta store. Promoting this
> catalog to its own replicated Raft group (so the mapping is consistent across
> the whole cluster) is the 4B/4C task; the `ClusterMetaStore` API is written so
> the storage backend can be swapped without touching callers.

## 5. API (`src/cluster/cluster_meta_store.h`)

```
Init(store, txn, config, create_if_not_exist)

RegisterShard(txn, ShardInfo)      SetShardState(txn, id, state)
RemoveShard(txn, id)               GetShard(id) / ListShards() / ShardCount()

PutGraphPlacement(txn, name, shard, state[, out_version])
DeleteGraphPlacement(txn, name)
GetGraphPlacement(name | id)       GetGraphName(id)
HasGraph(name)                     GraphCount() / GraphCountOnShard(shard)

Version() / SetVersion(txn, v)     MemoryFootprint()
```

All mutating calls also bump the cluster `ConfigVersion`, which the router
(4C) uses to detect and reject stale graph→shard mappings.

## 6. Shard lifecycle (Phase 4B)

`ShardManager` (`src/cluster/shard_manager.h/.cpp`) wraps the store with:

- **Registration** with validation (non-invalid id, non-empty name and
  endpoints). A successful registration seeds the first heartbeat.
- **Liveness**: `Heartbeat(id)`, `IsHealthy(id, now)`, `HealthyShards(now)`,
  `LastHeartbeat(id)`. Health lives only in memory (one `int64` per shard) and
  is never persisted. A shard is healthy when it is `ONLINE` and its last
  heartbeat is within `heartbeat_timeout_ms` (default 30 s).
- **Placement selection**: `PickShard(now)` chooses a shard for a new graph
  among healthy ONLINE shards by `LEAST_GRAPH_COUNT` (default) or
  `ROUND_ROBIN`. Least-count uses the store's incremental per-shard counter, so
  it is O(#shards), not O(#graphs).
- **Deregistration** delegates to the store's refuse-while-graphs-remain rule.

The clock is injectable (`SetClock`) so tests are deterministic.

### Per-shard count index

`ClusterMetaStore` now maintains `shard_counts_` (one entry per shard), updated
on every placement create/move/delete, so `GraphCountOnShard` and least-loaded
placement are O(1)/O(#shards). `MemoryFootprint()` is unaffected in practice
(a handful of shards).

## 7. Routing (Phase 4C)

`Router` (`src/cluster/router.h/.cpp`) turns the catalog into routing
decisions:

- `Resolve(graph, now)` → `RouteTarget { graph_id, shard_id, endpoint,
  placement_version }`, or a `RouteStatus`: `GRAPH_NOT_FOUND`,
  `PLACEMENT_NOT_ACTIVE` (CREATING/MOVING/DELETING), `NO_HEALTHY_SHARD`,
  `STALE_PLACEMENT`.
- `Validate(graph, seen_version, now)` rejects a request whose caller saw an
  older placement version, while still returning the current target so the
  caller can retry there — the guard against a stale router writing to an
  obsolete location.
- **Versioned, bounded cache**: keyed by `GraphId`, entries carry the
  `placement_version` and a TTL. A catalog version bump invalidates an entry on
  next access. The cache is hard-capped (`max_cache_entries`, default 64k) and
  dropped wholesale at the cap, so router memory stays bounded regardless of
  graph count. It holds only *hot* graphs; the catalog is the source of truth.
- **Leader location** is abstracted behind `ShardLocator` so the routing logic
  is unit-testable and the production implementation (query the shard's
  `dbms.ha.clusterInfo`) can be added without touching the router. When the
  locator is unknown the router falls back to the shard's first registered
  endpoint.

The router is deliberately stateless about bytes: it returns an endpoint and
lets the caller forward. A router loss therefore cannot affect persistent graph
state, satisfying that Phase 4 criterion by construction.

## 8. Files

| File | Contents |
|---|---|
| `src/cluster/cluster_types.h` | POD types, enums, `ShardInfo` serialization |
| `src/cluster/cluster_meta_store.h/.cpp` | the catalog (durable + compact index) |
| `src/cluster/shard_manager.h/.cpp` | shard registration, health, placement |
| `src/cluster/router.h/.cpp` | graph→shard→leader routing, versioned cache |
| `src/cluster/cluster_control.h/.cpp` | control-plane facade: create+auto-place, resolve, **receiver-side fence** |
| `test/test_cluster_meta_store.cpp` | CRUD, reload, version, footprint tests |
| `test/test_shard_manager.cpp` | registration, health, placement, dereg tests |
| `test/test_router.cpp` | resolve, cache/TTL, stale detection, health, bounds |
| `src/BuildLGraphApi.cmake` | adds `LGRAPH_CLUSTER_SRC` to `liblgraph` |
| `test/CMakeLists.txt` | registers the unit tests |

## 9. Control facade and receiver-side fencing (Phase 4C.1)

`ClusterControl` (`src/cluster/cluster_control.h/.cpp`) composes the store,
`ShardManager` and `Router` into the operations a server or admin procedure
needs: register/remove/list shards, create a graph on a healthy shard chosen by
the placement strategy, delete, inspect placement, and resolve by logical name.

It also provides the **receiver-side placement fence** the review requires:

```cpp
bool Fence(name, expected_version, *current);   // false if expected < current
```

Router-side version comparison protects a router's own decisions but cannot stop
an obsolete destination from accepting a write reached directly or with a stale
cache. A shard must therefore call `Fence()` at its write boundary and reject an
operation whose `expected` placement version is behind the shard's authoritative
placement. *Wiring `Fence()` into the per-shard write path is the remaining
integration step.*

## 10. Next steps

- **4C.2** — a real `ShardLocator` (query a shard's `dbms.ha.clusterInfo` for
  its leader) and a `Forwarder` that sends the `LGraphRequest` to the resolved
  endpoint, then wire the router in front of the server's request path, and call
  `ClusterControl::Fence()` at the receiving shard's write boundary.
- **4B.2** — promote the catalog behind a replicated control-plane Raft group so
  the mapping is consistent cluster-wide (the store API is backend-agnostic).
- **4D–4G** — richer placement strategies (disk/utilization weights), admin
  procedures, client transparency, multi-shard test harness and metrics.

## 11. Document history

| Date | Author | Change |
|---|---|---|
| 2026-09-20 | Phase 4A | Initial cluster metadata model and store |
| 2026-09-20 | Phase 4B | Shard lifecycle: registration, health, placement |
| 2026-09-20 | Phase 4C | Router: resolve/validate, versioned bounded cache |
| 2026-09-20 | Phase 4C.1 | ClusterControl facade + receiver-side placement fence |
