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

## 6. Phase 4A files

| File | Contents |
|---|---|
| `src/cluster/cluster_types.h` | POD types, enums, `ShardInfo` serialization |
| `src/cluster/cluster_meta_store.h/.cpp` | the catalog (durable + compact index) |
| `test/test_cluster_meta_store.cpp` | CRUD, reload, version, footprint tests |
| `src/BuildLGraphApi.cmake` | adds `LGRAPH_CLUSTER_SRC` to `liblgraph` |
| `test/CMakeLists.txt` | registers the unit test |

## 7. Next steps

- **4B** — shard abstraction + registration/heartbeat; make `ShardInfo` the
  source of truth for endpoints/health; move the catalog behind a replicated
  control-plane Raft group.
- **4C** — the router: resolve graph→shard→leader, cache keyed by
  `placement_version`, refresh on mismatch, forward via the existing RPC.
- **4D–4G** — placement strategies, admin procedures, client transparency,
  multi-shard test harness and metrics.

## 8. Document history

| Date | Author | Change |
|---|---|---|
| 2026-09-20 | Phase 4A | Initial cluster metadata model and store |
