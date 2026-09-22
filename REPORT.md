# Development Status Report — GraphFin

**For:** lead software engineer and architect review
**Branch:** `perf-series-merge`
**Date:** 2026-09-22
**Scope:** merged performance/time-series candidate, release qualification, and
the historical whole-graph sharding control-plane work.

> This report contains historical control-plane evidence from the performance
> workstream. It is not a release sign-off. For the current candidate gate,
> use [RELEASE.md](RELEASE.md) and [TASK.md](TASK.md). The latest merged HA
> series failover run failed both cases; the candidate remains unsigned-off.

---

## 1. Executive summary

The tree contains a substantial, unit-tested **whole-graph sharding control plane**
(cluster metadata, shard lifecycle, placement, routing, migration state) on top
of the previously delivered multi-graph scaling (lazy loading, 100k graphs) and
a hardened 3-node HA replica group (chaos-tested, soak-validated).

**Historical control-plane evidence reported by the performance workstream:**
- 34/34 cluster unit tests pass on the build host, including transaction-scoped
  staging, fencing, placement strategies, migration lifecycle, and a bounded
  memory-footprint assertion.
- 11/11 HA chaos integration tests pass repeatedly (5 consecutive full-suite
  runs observed); a 3-node soak reached 300 kills / 300 acked / 0 failures, and
  a 3-shard soak reached 404 kills with per-replica reconciliation.
- A 24-hour Phase 3 acceptance soak was reported by the historical workstream;
  it is not current merged-release evidence and must not be reused as the
  current candidate's sign-off artifact.

**Current merged-candidate evidence (2026-09-22, fixed candidate):** the strict
C++ unit gate passed 110/110 (evidence `unit-p6uuknon`), the joint
financial/telemetry smoke gate passed 4/4 (`smoke-sh_x0e7u`), the live client
gate passed 14/14 with `neo4j==4.4.6` (`clients-gjxcjgat`), and the HA series
failover gate passed 2/2 with zero skips and zero failures (`ha-aha_pbu5`).
All four ran on the same source/build snapshot: base commit `701fbfb9` plus
the recorded dirty-content manifest, `lgraph_server`
`5010f5ecabbefbe3162483c500e8c06c326152fc95a4a21e721fafd40485d178`,
`unit_test`
`3e3d33a349ec17a75e1e2649bbde65ceb8feca79545dad92439a0910808a0e38`,
compile image
`sha256:2350a9a1f998b6898b169d28015c60c49b488def8980b9553a657819763afedf`.
The candidate must still be frozen into an immutable commit and re-qualified
clean before any tag, package, or publication step.

**What is explicitly not done:** live multi-shard request forwarding, a
replicated control plane, receiver-side fence wiring into the server write path,
admin procedures, migration data movement, production hardening, and
intra-graph sharding.

---

## HA series failover blocker — diagnosis and fix (2026-09-22)

The merged HA series gate (`test_merge_series_ha.py`, MERGE-09 baseline) failed
every strict run until this fix, with varying symptoms across runs: `Not a
leader` on graph creation, `No such graph` on schema writes, and exact-value
reconciliation timeouts with `Vertex 0 has no time series field [samples]` on
followers. Investigation separated a harness routing defect from a product
replication defect; neither was converted into a skip and the oracle (exact
per-replica value reconciliation after leader loss and restart) is unchanged.

Harness fix (test-only, `test_merge_series_ha.py`, `ha_util.py`): the
`cypher_on_leader` helper pinned the first live node with no rotation, so a
transient login failure or leadership change produced 90 s of follower
redirects. Graph creation now uses explicit leader discovery plus
`callCypherToLeader` on a leader-connected client, waits until the new graph
is listed before seeding, and every retry client is logged out.

Product fix (`src/cypher/parser/clause.h`, `QueryPart::ReadOnly`): a
standalone `CALL <mutating-procedure> ... YIELD ... RETURN ...` statement
parses as a regular query whose procedure call lands in `iq_call_clause`,
which the v1 read-only decider ignored. HA therefore classified these writes
as reads and executed them leader-local without raft replication. Evidence:
the raft commit index did not advance for
`CALL db.createSeriesField(...) YIELD field RETURN field` (advance +0 on all
replicas) while the identical statement without trailing `RETURN` advanced it
(+1 everywhere); follower server logs show `AlterLabelAddFields` never ran
there. The fix checks `iq_call_clause` exactly like `sa_call_clause`.
Regression coverage:
`TestSeriesTransaction.MutatingProcedureCallsWithReturnClassifyAsWrites`
(classifier expectations for series DDL, in-query appends/CAS, and pure
reads). Incremental rebuilds reused the existing tree (client/tool relink
~137 s in `phase0-logs/build-20260922-181414.log`; engine-fix rebuild with
full unit-test relink in `phase0-logs/build-20260922-185044.log`; no clean
rebuilds).

Result: strict HA gate 2/2 passed, zero skips, zero failures
(`/tmp/graphfin-merge-results/ha-aha_pbu5`), on the same binaries that pass
unit (110/110), smoke (4/4), and clients (14/14). The current merged-release HA evidence is the failed
series failover gate recorded above; the historical soak does not close it.

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

### 3.1 Transaction-owned catalog batch (REVIEW R1/R2/R10 fixed)
All mutations go through `ClusterMetaStore::Batch` (and `MigrationManager::Batch`
for migration records). A Batch owns one `KvTransaction`, holds the store-wide
writer lock from construction until `Commit()`/`Abort()`, stages
transaction-locally (never store-global), writes durable rows, then publishes to
memory only after the durable commit succeeds — one `Commit()` entry point with
RAII abort. Readers observe committed state only; an aborted batch publishes
nothing. `SetShardState` publishes the full shard descriptor including the bumped
`config_version` (R10). Regressions: `StagedBatchesAreTransactionOwned`,
`ShardStatePublishesFullDescriptor`, `AbortLeavesNoVisibleMigration`,
`BatchSeesOwnWritesForValidation`.

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

### 3.5 Ownership guard at the receiver (REVIEW R4 fixed at the control layer)
`Fence`/`FenceAt(shard, name, expected_uid, expected_version)` accept only an
exact incarnation + epoch match on the authoritative owner in ACTIVE serving
state, and always report the authoritative `(uid, version)` for retry. Stale
epochs, future epochs (catalog lag refreshes, never jumps ahead), wrong UIDs,
wrong shards, missing graphs, and non-ACTIVE placements all fail closed. A
partitioned source holding old metadata presents a non-equal tuple and cannot
pass. `ClusterRequestHandler::Handle` carries the UID end-to-end (recreate →
`REJECT_STALE`). **Not yet wired** into the per-shard server write path — that
is the 4C.2 integration step. Regressions: `FenceRejectsStaleFutureWrongShardAndNonActive`,
`RecreatedIncarnationAndFutureEpochRejected`.

### 3.6 Immutable graph identity (REVIEW R3 fixed)
`GraphPlacement` is 24 B with a persisted, never-reused `unique_id`; `GraphId`
remains a dense, process-local index. Every new incarnation — cross-batch,
same-batch delete/recreate, and across restarts — allocates a fresh UID
(`RecreateAllocatesFreshUid`, `RecreateRetiresOldIncarnation`,
`CacheInvalidatedByRecreate`). `RouteTarget` carries the UID; the router cache
is keyed to the incarnation; `Validate` and the fence compare UID + version.

### 3.7 Attempt-bound moves (REVIEW R7 fixed at the control/state-machine layer)
`CompleteMove`/`AbortMove` compare-and-set against the `(uid, MOVING version)`
attempt tuple: mismatch returns `STALE_PLACEMENT`, so a delayed completion or
stale abort from one attempt can never cut over or cancel a later attempt.
Applied attempts replay idempotently (`MoveCompletionIsAttemptBound`).
`MigrationManager` transitions accept an `expected_id`, the migration-id
allocator is persisted independently of prunable records (never reused:
`AttemptBindingAndAllocatorSurvivesPrune`), and post-cutover states must go
through `ROLLBACK` instead of jumping terminally to `FAILED`. `MOVING` remains
an offline prototype (not routable for the duration); online copy/catch-up is
future work. Server-side fencing enforcement for cutoverVs queued writes is
covered by `FenceRejectsStaleFutureWrongShardAndNonActive`.

### 3.8 Memory posture (partial estimate; R9 remains)
`MemoryFootprint()` measures the committed index only (placements + names +
buckets + shards; asserted < 100 B/graph at 20k graphs). It excludes per-Batch
staging, hash overhead beyond capacity, tombstones, and migration records.
100k-graph RSS/churn measurement with staging/tombstone accounting is the R9
follow-up, not claimed here. Router cache capped; refcounts optionally compact
(`-DLGRAPH_COMPACT_REFCOUNT=1`).

---

## 4. Validation evidence (with provenance)

| Artifact | Result | Where |
|---|---|---|
| Cluster unit tests (7 suites) | **syntax-checked; full run pending** (was 34/34; +11 new R1–R4/R7/R8/R10 regressions: `StagedBatchesAreTransactionOwned`, `ShardStatePublishesFullDescriptor`, `AbortLeavesNoVisibleMigration`, `BatchSeesOwnWritesForValidation`, `RecreateAllocatesFreshUid`, `RecreateRetiresOldIncarnation`, `FenceRejectsStaleFutureWrongShardAndNonActive`, `MoveCompletionIsAttemptBound`, `AttemptBindingAndAllocatorSurvivesPrune`, `CacheInvalidatedByRecreate`, `RecreatedIncarnationAndFutureEpochRejected`) | `g++ -fsyntax-only` clean on all cluster sources + 6 test files in `tugraph-compile-arm64:local`; `unit_test` build/run is the merge gate |
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
