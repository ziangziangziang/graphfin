# Development Status Report — TuGraph Scaling Project

**For:** lead software engineer and architect review
**Branch:** `performance` · **HEAD:** `60b32355b`
**Date:** 2026-09-21
**Scope:** Phases 0–4 (control plane), Phase 3 HA hardening, Phase 5 foundations,
memory review, and resolution of `REVIEW.md` findings 1–6.

---

## 1. Executive summary

The tree contains a complete, unit-tested **whole-graph sharding control plane**
(cluster metadata, shard lifecycle, placement, routing, migration state) on top
of the previously delivered multi-graph scaling (lazy loading, 100k graphs) and
a hardened 3-node HA replica group (chaos-tested, soak-validated).

**What is proven (with evidence):**
- 34/34 cluster unit tests pass on the build host, including transaction-scoped
  staging, fencing, placement strategies, migration lifecycle, and a bounded
  memory-footprint assertion.
- 11/11 HA chaos integration tests pass repeatedly (5 consecutive full-suite
  runs observed); a 3-node soak reached 300 kills / 300 acked / 0 failures, and
  a 3-shard soak reached 404 kills with per-replica reconciliation.
- A **24-hour Phase 3 acceptance soak is running now** on the test host.

**What is explicitly not done:** live multi-shard request forwarding, a
replicated control plane, receiver-side fence wiring into the server write path,
admin procedures, migration data movement, production hardening, and
intra-graph sharding. The 24–72 h acceptance result is pending the running soak.

---

## 2. Commit map (this branch, oldest → newest relevant)

| Commit | Contents |
|---|---|
| `488ad42cc` | Phase 2 close-out (lifecycle/eviction tests) |
| `598a7f1f5` | Galaxy-owned eviction task, per-record CREATE, test staging |
| `fd7e996b8` | Phase 3: HA hardening, observability, chaos harness |
| `6e8d8b894` | Phase 4A: cluster metadata model + `ClusterMetaStore` |
| `8fd6675a3` | Phase 4B: shard lifecycle (registration, health, placement) |
| `1fdb9e39f` | Phase 4C: router with versioned bounded cache |
| `87781a7c0` | Phase 4D: weighted placement strategy |
| `e88b6b99a` | `ClusterControl` facade + receiver-side fence |
| `9ed0ef93d` … `e89154ca9` | Staging test, config-version stamping, cache-invalidation test |
| `7b3861a65` | Memory review + opt-in compact refcounts + footprint estimate |
| `1d13b5ade`, `e89154ca9` | `REVIEW.md` findings 1, 3, 4, 5, 6 (+ soak ledger) |
| `eed0b7b31` | Immutable `graph_uid` |
| `7cd684fe2`, `80452b13a` | `MigrationManager` state machine + metrics/planner |
| `0d43f468d` | `ClusterRequestHandler` routing core |
| `a2de0efde` | Placement move lifecycle (`Begin/Complete/AbortMove`) |
| `3d38bc854` | Doc reconciliation (retired stale claims) |
| `60b32355b` | GCC-8 build fixes for the above |

---

## 3. Architecture decisions (for review)

### 3.1 Transaction-scoped catalog publication (finding 1)
`ClusterMetaStore` mutators write durable tables and **stage**; the in-memory
index is published only by `CommitStaged()` (after `txn->Commit()`) and
discarded by `RollbackStaged()`. Readers never observe uncommitted placements.
Trade-off: callers must pair every mutating batch with commit-then-publish
(the unit tests do); there is no implicit publication.

### 3.2 Single authoritative mutation order per shard (finding 2)
`docs/architecture/09-write-path-audit.md` §6 records the decision: a
deployment selects exactly one write-replication path (legacy braft HA **or**
Bolt HA), and the server warns when both are enabled. Full unification is
scoped to Phase 6, not Phase 7.

### 3.3 Bounded open-graph admission (finding 4)
`GetGraphRef` waits up to `--graph_open_admission_timeout_s` (default 30 s)
for a lease, then fails with a retryable `Timeout` instead of exceeding
`max_open_graphs`. Leases are never evicted. `TestGalaxy.OpenGraphAdmissionBound`
pins one graph at capacity 1 and asserts failure-then-success.

### 3.4 Router checks health on every resolve (finding 6)
`Router::Resolve` verifies shard existence/health and the shard config version
on **every** call (cache hits included); leader discovery runs outside the
router mutex; the cache is hard-capped (default 64k, drop-wholesale). Shard
registration/state changes stamp a fresh `config_version` so warm entries
invalidate.

### 3.5 Receiver-side fencing (review requirement)
`ClusterControl::FenceAt(shard, name, expected)` rejects when the receiver does
not host the graph **or** the sender is behind the authoritative version, and
always reports the current version for retry routing. **Not yet wired** into
the per-shard server write path — that is the 4C.2 integration step.

### 3.6 Immutable graph identity
`GraphPlacement` grew 16 → 24 B for a persisted, never-reused `unique_id`.
`GraphId` remains a dense, process-local index. Routing, migration, and fencing
key on `unique_id`.

### 3.7 Memory posture
Catalog index ~6 MiB at 100k graphs (asserted < 100 B/graph); router cache
capped; refcounts optionally compact (`-DLGRAPH_COMPACT_REFCOUNT=1`, ~60 KiB →
~7.5 KiB per open graph). The web console streams from disk (no RAM lever).
`lmdb_max_dbs` is lazily faulted (~50 KiB resident impact).

---

## 4. Validation evidence (with provenance)

| Artifact | Result | Where |
|---|---|---|
| Cluster unit tests (7 suites) | **34/34 pass** | `unit_test --gtest_filter='TestClusterMetaStore.*:TestShardManager.*:TestRouter.*:TestClusterControl.*:TestMigrationManager.*:TestRequestRouter.*:TestGalaxy.OpenGraphAdmissionBound'`, compact `-O0` build on `tugraph/tugraph-compile-centos7` |
| HA chaos suite (13 tests incl. fixture) | **11/11 pass ×6 consecutive full runs** | `test/integration/test_ha_chaos.py` via `~/run-ha-test.sh` |
| 3-node soak | **300 kills / 300 acked / 0 failed** | `~/soak-long.log` |
| 3-shard soak | **404 kills, all counters reconciled** | `~/test-queue.log` (`QUEUE DONE`, all stage exits 0) |
| 24 h acceptance soak | **running** (tmux `soak24`, `~/soak24.log`) | 3-node, kill every ~30 s, ledger + per-replica reconciliation |
| 9-node footprint | **~926 MiB** (~308 MiB per 3-node shard) | soak `res:` lines |

Build: `cmake -DOURSYSTEM=centos7 -DCMAKE_BUILD_TYPE=Release
-DCMAKE_CXX_FLAGS_RELEASE="-O0 -g0 -DNDEBUG" -DLGRAPH_COMPACT_REFCOUNT=ON
-DBUILD_PROCEDURE=OFF -DWITH_TESTS=ON`, `make -j1 lgraph unit_test`
(2-core/2 GiB host; `-j1 -O0` required to avoid OOM).

---

## 5. Known gaps, risks, and blocked items

| Item | Status |
|---|---|
| Live multi-shard request forwarding + admin procedures + replicated control plane | **Not implemented** (4C.2/4B.2/4E). Components + tests exist; server wiring pending. |
| Receiver-side fence wiring into the write path | Designed (`FenceAt`) and unit-tested; **not wired**. |
| Migration data movement (snapshot ship + catch-up) | **Not implemented**; state machine + planner + move lifecycle done. Deferred per REVIEW §4 until E2E sharding is reliable. |
| Real network partitions | **Blocked**: test host has no sudo (`tc`/`iptables` impossible). Kill-based proxy in place. |
| Intermittent upstream SIGSEGV (F3) | **Open**; diagnosis plan recorded (loop `TestBackupRestore` with container-local cores; ASAN build). |
| 24–72 h soak | **24 h running**; 72 h extension is a follow-up decision. |
| Reproducible arm64 image | **Known gap**; x86_64 from-source script added (`ci/phase0/build_image.sh`). |
| Production hardening (SLOs live, dashboards, quotas, upgrades) | Docs drafted; implementation pending. |
| Intra-graph sharding | Deferred by design with entry criteria. |

---

## 6. Questions for the reviewers

1. **Staging API**: is explicit `CommitStaged()`/`RollbackStaged()` after `txn->Commit()` acceptable for the control plane, or should publication be implicit (e.g. a commit hook)? The current design favors explicitness at the cost of call-site discipline.
2. **Single-path-per-shard contract**: is documenting + warning the right scope for now, or should mixed legacy+Bolt HA be a hard startup error? A hard error could break existing mixed deployments.
3. **`unique_id` width**: is a 64-bit monotonic id (24-byte placement) acceptable, or should we budget 32 bits to keep placements at 20 bytes?
4. **Fence placement**: should `FenceAt` live in `ClusterControl` (as now) or move into the storage apply path so *every* write (including Bolt-applied Cypher) is fenced without each caller remembering?
5. **Partition acceptance**: is kill-based chaos sufficient for Phase 3 acceptance, or must we provision a host with sudo for true `tc`-based partitions before sign-off?
6. **Bolt HA future**: deprecate/unify it, or keep it as a supported second path alongside legacy HA? The decision affects 4C.2 scope.
7. **9-node E2E gating**: is a live 3-shard × 30k-graph run on a larger host a merge requirement for Phase 4, or is component + single-shard evidence enough to proceed to migration design?
8. **Compact refcounts**: keep as opt-in (`OFF` default), or make it the default given the fleet's memory constraints? It trades false-sharing performance for 8× memory.
