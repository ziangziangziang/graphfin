# 12 — Whole-Graph Sharding: Implementation Status (Phase 4)

This document maps `PROJECT.md` Phase 4 success criteria to what is
implemented, tested, and still outstanding, and specifies the remaining work.

## 1. Scope delivered (Phase 4A–4D)

| Sub-phase | Deliverable | Files | Tests |
|---|---|---|---|
| 4A | Cluster metadata model + durable, memory-frugal catalog | `src/cluster/cluster_types.h`, `cluster_meta_store.{h,cpp}` | `test_cluster_meta_store.cpp` |
| 4B | Shard lifecycle: registration, health, placement selection | `cluster/shard_manager.{h,cpp}` | `test_shard_manager.cpp` |
| 4C | Router: resolve/validate + versioned bounded cache | `cluster/router.{h,cpp}` | `test_router.cpp` |
| 4D | Placement strategies: least-count, round-robin, weighted | `shard_manager.*` | `test_shard_manager.cpp` |

All 14 cluster unit tests pass on the remote build. The catalog is
memory-optimised for the fleet: a flat 16-byte placement array, an interned
name arena, and an open-addressing name index (asserted < 100 B/graph; the
whole index is ~5 MiB at 100k graphs). The router cache is hard-capped so
router memory is bounded independently of graph count.

## 2. Phase 4 success criteria — status

| # | Criterion (PROJECT.md) | Status | Where |
|---|---|---|---|
| 1 | ≥ 3 independent shards operate simultaneously | ⚪ Not deployed | Logic ready; needs ≥ 9 processes (3 shards × 3 replicas) — see §4 |
| 2 | Each shard is a replicated group | ✅ Provided by Phase 3 | `HaStateMachine` per shard |
| 3 | ≥ 30,000 graphs distributed across the cluster | 🟡 Logic ready | `PickShard` + catalog proven at 20k in unit test; live distribution pending a larger host |
| 4 | Clients access graphs by logical identifier | 🟡 internal primitive only | `Router::Resolve(name, …)` exists, but no public endpoint routes by logical name; not demonstrated |
| 5 | Clients do not need shard addresses | 🟡 internal primitive only | Router returns an endpoint, but clients still connect to a shard directly |
| 6 | Reads/writes routed to the correct shard | 🟡 decision only, forwarding pending | `Router::Resolve`; forwarding is 4C.2 |
| 7 | Graph creation automatically chooses a shard | 🟡 internal primitive only | `ShardManager::PickShard` is not wired to `dbms.graph.createGraph` |
| 8 | Placement metadata survives restart | ✅ | `ClusterMetaStore` reload test |
| 9 | Stale routing metadata detected safely | 🟡 router-side only | `Router::Validate` rejects a stale caller, but the receiving shard does not fence writes (see §4.0) |
| 10 | Router failure does not affect persistent graph state | ✅ by construction | Router is stateless and moves no bytes |
| 11 | Shard-level failover remains functional | ✅ Provided by Phase 3 | Phase 3 replica group |
| 12 | No cross-shard query execution required | ✅ by model | A graph maps to exactly one shard; enforced by `GraphPlacement` |

Legend: ✅ done & tested · 🟡 component/primitive only, not demonstrated end-to-end · ⚪ not yet deployed.

**Correction (review):** an earlier version of this table marked criteria 4, 5 and 7
as complete based on internal primitives. They are useful primitives but do not
demonstrate the client-facing capability; they are now 🟡. Router-side version
comparison alone also cannot stop an obsolete destination from accepting writes —
fencing must be enforced at the receiving shard (see §4.0).

## 3. Honest summary

Phase 4's **control plane** (metadata, shard lifecycle, placement, routing
decisions, stale-detection) is implemented and unit-tested. What is **not** yet
done is the **data plane and deployment**: forwarding a live request to the
resolved shard, exposing the cluster admin surface, and running an actual
multi-shard cluster. Those require server/RPC integration and a host with more
than the current 2 vCPU / 2 GiB (a 9-process deployment does not fit).

## 4. Remaining Phase 4 work

0. **Receiver-side placement fencing (required before routing is safe).**
   A router that compares placement versions protects only its own decisions.
   An obsolete destination can still accept a write if a client/router reaches
   it directly (or with a stale cache). Each shard must therefore fence at its
   own write boundary: carry the graph's expected `placement_version` (or a
   monotonic placement epoch) on forwarded writes and reject a write whose
   version is behind the shard's authoritative placement for that graph file.
   Until this exists, "stale routing detected safely" is only partly met.

1. **4C.2 — Router integration (highest priority)**
   - Real `ShardLocator`: query a shard's `dbms.ha.clusterInfo()` for its leader
     and cache the result.
   - `Forwarder`: send the `LGraphRequest` to `RouteTarget.endpoint` (reuse the
     existing brpc client), and place the router in front of the server request
     path for graphs whose placement is not local.
   - Reject/retry on `STALE_PLACEMENT` by refreshing and re-resolving once.
2. **4B.2 — Control-plane consistency**: promote `ClusterMetaStore` behind its
   own replicated Raft group (the store API is backend-agnostic) so the catalog
   is consistent cluster-wide.
3. **4E — Admin surface**: Cypher procedures (`dbms.cluster.registerShard`,
   `removeShard`, `listShards`, `inspectGraphPlacement`, `inspectShardHealth`)
   backed by `ShardManager`/`Router`; auto-placement on `dbms.graph.createGraph`.
4. **4F — Client transparency**: extend the Bolt `Route` message with shard/
   leader info; allow clients to talk only to the router endpoint.
5. **4G — Multi-shard harness & metrics**: a 3-shard × 3-replica integration
   test (separate host), plus routing metrics (hit/miss, stale rejections,
   per-shard counts).

## 5. Later phases (not started)

- **Phase 5** — Online graph migration & rebalancing: a migration state machine
  over the Phase 4 placement metadata, reusing Phase 3 snapshots.
- **Phase 6** — Production hardening / v1.0: SLOs, cluster metrics/alerts,
  backup at graph/shard scope, rolling upgrades, quotas, runbooks.
- **Phase 7** — Optional intra-graph sharding (a separate distributed-DB
  program; keep the whole-graph architecture intact).

## 6. Deployment feasibility note

The Phase 4 success criteria that require a live cluster (≥ 3 shards, 30k
distributed graphs, shard failover) need a host that can run 9 `lgraph_server`
processes plus the build toolchain. The current test VM (2 vCPU / 2 GiB) runs a
3-node group for the Phase 3 soak but cannot host a 9-process sharded
deployment. Provide a larger host (e.g. 8 vCPU / 16 GiB) to execute the Phase 4
integration suite.

## 7. Document history

| Date | Author | Change |
|---|---|---|
| 2026-09-20 | Phase 4 | Initial sharding status and criteria mapping |
