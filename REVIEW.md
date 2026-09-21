# Architecture and Engineering Review

**Date:** 2026-09-21

**Reviewed branch / HEAD:** `performance` / `4dd816d83`

**Implementation revision cited by REPORT.md:** `60b32355b`

**Disposition:** Continue development, but do not sign off Phase 3 acceptance, Phase 4 completion, or production readiness yet.

The whole-graph approach remains appropriate. The compact catalog, bounded router cache, graph admission limit, and separation of placement decisions from transport are useful foundations. The next milestone should be a correct, integrated sharding path. Migration data movement should wait until that path is demonstrated.

The report is candid about missing integration, but “complete, unit-tested control plane” overstates the implementation. There are correctness defects in transaction publication, graph identity, and migration state, in addition to the acknowledged replication and server-wiring gaps. The existing soak checks also cannot establish the claimed absence of acknowledged-write loss.

## 1. Scope and evidence

I reviewed `REPORT.md`, the project acceptance criteria, architecture notes, all cluster components and their unit tests, server startup guards, graph admission/eviction code, and the HA chaos/soak drivers. The working tree was clean at the start. This review changes only this document.

The six cluster test files contain 33 tests; adding `TestGalaxy.OpenGraphAdmissionBound` matches the reported 34. This confirms the inventory, not a fresh test pass. The local `unit_test` is a Linux AArch64 executable from an older build; I did not rebuild or rerun the C++ suite. Remote logs and the currently running soak were not independently inspected. Their results remain developer-reported evidence, and the SIGSEGV described as “upstream” remains unattributed until reproduced and isolated.

I did execute the checked-in `ha_soak.py` with a mocked HA backend and simulated clock. The backend acknowledged write 1, lost it during restart, then accepted write 2. The actual driver returned **exit 0**, with two acknowledgements and final counter 2. This verifies a test-oracle defect; it does not demonstrate a database durability failure.

| Area | Assessment |
|---|---|
| Phases 0–2 | Existing scaling foundation; bounded admission is visible in code. No fresh 100k-graph or performance certification in this review. |
| Phase 3 | Useful kill/restart evidence; acceptance remains open for write-path coverage, partitions, trustworthy long-soak reconciliation, and crash diagnosis. |
| Phase 4 | Component prototype with unit coverage. No replicated catalog or working public routing path; correctness fixes are required before integration. |
| Phase 5 | State-machine and planner scaffolding. Neither safe restartability nor online migration is established. |
| Phases 6–7 | Keep production hardening and intra-graph sharding as separate later milestones. |

## 2. Findings requiring action

Priorities below describe the next relevant gate: **P0** blocks HA acceptance or safe cluster enablement; **P1** must be fixed before the affected component is integrated; **P2** is required before claiming the corresponding scale or operational behavior. These are new review identifiers, not the absent earlier REVIEW.md's numbering.

### R1 — P1: Catalog staging is store-global, not transaction-scoped

**Evidence:** `src/cluster/cluster_meta_store.h` stores one `pending_` and `staged_version_`; `cluster_meta_store.cpp:540–575` publishes or clears the whole batch without identifying its transaction.

Even with LMDB's single writer, the API has a race: transaction A commits and releases the database writer lock; transaction B starts and stages changes before A calls `CommitStaged()`. A then publishes B's uncommitted mutations too. If B aborts, rollback cannot undo those published changes. Per-method locking does not cover this commit/publication interval. The concurrent-reader test exercises one writer before commit, not this interleaving.

**Recommendation:** Introduce a transaction-owned batch with one owner and an enforced writer serialization boundary spanning mutation, durable commit, and publication. Give it RAII rollback and one commit entry point. A commit hook is acceptable if it guarantees the same ownership and ordering; a hook alone is insufficient. Prepare publication so allocation failure after durable commit cannot leave a partially updated live index; otherwise fail closed and reload committed state.

**Required regression:** Pause A after durable commit, attempt B's mutation, publish A, then abort B. B must never become visible. Also cover commit failure and exceptions during publication.

### R2 — P1: Migration state is exposed before commit and survives abort in memory

**Evidence:** `src/cluster/migration_manager.cpp:140–174` updates `migrations_` inside `Begin`/`Advance`, while the caller still owns the uncommitted transaction. `Fail`, `Cancel`, `Prune`, and progress updates have the same problem. `Advance` changes memory even before `PersistLocked` returns.

For example, `Begin(txn, uid, ...)` followed by `txn.Abort()` leaves an active migration in memory with no durable record. Aborting an advance leaves the running process ahead of its restart state. A worker reading that state could perform irreversible copy, cutover, or cleanup actions. Writing a table inside a transaction is not durable commitment.

**Recommendation:** Use the transaction/publication mechanism from R1 for both placement and migration records. Workers must observe committed state only. Commit related placement and migration transitions atomically.

**Required regression:** Abort and inject persistence/commit failures for each mutator; compare the visible state with a newly opened manager. Test crash recovery at each committed transition, not only a successful final reload.

### R3 — P1: Delete/recreate reuses graph identity, and routing does not carry it

**Evidence:** `cluster_meta_store.cpp:362–363` preserves the UID whenever `IndexFind` succeeds. `ApplyDeleteGraph` at line 529 leaves the name and UID indexed as a tombstone. Consequently, create `g`, delete it, and recreate `g` without restarting: the new graph gets the old UID. After a reload, the deleted durable row is absent and the same operation allocates a new UID instead.

This makes graph incarnation depend on process lifetime and allows an old migration record to alias a new graph. The current UID test covers moves, reloads, and a different new name; it omits same-name recreation. Also, contrary to REPORT §3.6, `RouteTarget` in `src/cluster/router.h:32` contains only process-local `graph_id`, shard, endpoint, and version. `FenceAt` accepts a name and version, not a UID.

**Recommendation:** Allocate a new 64-bit UID for every new incarnation, including same-batch delete/recreate. Carry and validate UID plus placement epoch throughout routing, forwarding, migration, and receiver admission. Keep dense `GraphId` internal to a catalog snapshot.

**Required regression:** Delete/recreate before and after restart and within one batch; deliver delayed operations bearing the old UID and verify rejection.

### R4 — P0: Receiver fencing needs an ownership protocol, not just a version helper

**Evidence:** `src/cluster/cluster_control.cpp:135–146` accepts `expected >= current` and does not require `ACTIVE`. `request_router.cpp:75` invokes this helper before returning `HANDLE_LOCALLY`; it does not hold an ownership guard through execution. No live server write path invokes this component.

A future epoch is accepted locally, whereas the forwarding branch requires equality. A lagging source catalog can therefore accept a request from a newer epoch. Calling `FenceAt` directly on a `MOVING` placement with its current version also succeeds, despite the router rejecting non-active placements. Checking before execution does not prevent ownership changing between the check and the write.

**Recommendation:** Define a receiver admission token containing graph UID, owner, and epoch. Require the expected committed epoch and a serving state; refresh or reject when local metadata is behind. Order ownership revocation and accepted writes through the shard's authoritative mutation log, and persist the local fence. Cutover must stop/drain source writes before destination activation. A replicated catalog alone cannot fence a partitioned source that retains old metadata.

Keep `ClusterControl` as the policy/query facade, but enforce admission in the common replicated write path. Apply/replay must use durable ordered fencing state, not perform nondeterministic lookups against a remote catalog. Inventory every mutation surface, including Bolt, RPC/REST, import, schema, plugins, graph lifecycle, and restore.

**Required regression:** Old and future epochs, wrong UID/shard, non-active state, catalog lag, direct writes bypassing the router, and cutover racing with a queued write. Once integrated, add partitioned-source tests.

### R5 — P0: The single-replication-path contract is still unenforced

**Evidence:** `src/server/lgraph_server.cpp:123–138` rejects legacy HA plus Bolt without Bolt Raft, then only warns when both replication engines are enabled. The audited Bolt-only configuration also leaves mutation surfaces outside its replicated path. This is acknowledged in `docs/architecture/09-write-path-audit.md`.

A warning does not establish one mutation order. Two independent logs can order conflicting schema/data/admin writes differently across replicas. Documentation cannot close this finding.

**Recommendation:** Fail startup for configurations that expose conflicting write paths or unreplicated supported writes. The immediate supported baseline should be legacy HA with Bolt disabled, until Bolt writes can enter the same authoritative path. If a restricted Bolt-only mode is retained, disable unsupported mutation surfaces explicitly and verify its coverage. Publish the supported configuration matrix and migration instructions for existing mixed deployments.

**Required regression:** Startup matrix tests plus cross-surface ordering tests on each supported configuration. Compatibility concerns justify a transition plan, not continued acceptance of divergent writes.

### R6 — P0: The soak oracle can miss acknowledged-write loss

**Evidence:** `test/integration/ha_soak.py:87–123` writes absolute counter values and checks only the final value through an available node. It has neither a per-operation ledger nor per-replica reconciliation. `ha_shard_soak.py:160–188` logs acknowledgements but still compares only the final counter on each replica. Logging intermediate values does not preserve evidence of their application in the database.

Counterexample, also verified with the mock execution above: acknowledge `SET value=1`, lose that write, then acknowledge `SET value=2`. Both drivers' final-value invariant can pass. Writes occur after a kill, with a delay, rather than continuously overlapping failures. These runs exercise recovery and final convergence, but do not prove all acknowledged writes survived.

**Recommendation:** Store a unique immutable operation ID and payload/checksum for each write. Durably record client acknowledgements outside the cluster and reconcile every acknowledged ID and payload against each replica after bounded catch-up. Track ambiguous outcomes separately; retries need stable IDs and defined deduplication. Run writers concurrently with faults, including failure near acknowledgement and commit boundaries. Report availability failures separately from durability failures.

The claimed 24-hour run must identify its exact script, commit, binary/library checksums, configuration, start/end timestamps, and terminal exit status. If a remote driver contains stronger checks than the repository version, commit and review it. A longer run of the current oracle cannot close Phase 3 acceptance. Do not rebuild, replace libraries, or pause the test deployment during a qualifying continuous soak.

### R7 — P1: Move completion is not bound to a migration attempt

**Evidence:** `src/cluster/cluster_control.cpp:68–115` validates `dst` during `BeginMove` but does not persist it in the placement. `CompleteMove(name, dst)` checks only that the graph is currently `MOVING` and the supplied destination is online. It receives no expected epoch, UID, or migration ID, and has no connection to `MigrationManager`.

A delayed completion from an aborted attempt can complete a later attempt for the same name, even to a different destination. Similarly, a stale abort can cancel a newer move. `MigrationManager::Advance` keys only on graph UID, so an old worker can affect a replacement attempt. Repeating a successfully applied transition returns false rather than an identifiable idempotent success.

**Recommendation:** Persist a migration attempt ID, source, destination, UID, and expected placement epoch. Require compare-and-set transitions against that tuple, with explicit idempotent replay semantics. Do not mark a post-cutover failure terminal and permit another migration until ownership and cleanup have been reconciled. Persist the migration-ID allocator independently of prunable records.

The current `MOVING` state rejects all routed traffic for the duration of the move. Document that as an offline prototype. Online migration needs source-serving copy/catch-up phases and a bounded write-quiescence interval at cutover.

### R8 — P1: Batch decisions do not see their own writes

**Evidence:** `ClusterControl::CreateGraph` checks committed `HasGraph`; placement selection reads committed shard counts. `SetShardState` copies committed `shards_`, and `DeleteGraphPlacement` requires a committed name index entry.

In one transaction, two creates of the same name can both return OK, a newly created placement cannot be deleted, and repeated least-load placements can all choose the same initially empty shard. Registering a changed shard descriptor then setting its state can write the old endpoints/weight back to disk while publication preserves the new descriptor in memory.

**Recommendation:** Provide a transaction-local read/write view for validation, counts, descriptors, and identity allocation, with committed-only external reads. If batching is intentionally unsupported, enforce one operation per transaction in the API. The current interface and tests explicitly support multi-operation batches, so that restriction would be a contract change.

**Required regression:** Duplicate create, create/delete, register/state-update, and weighted bulk placement in one batch; compare published state with a reload.

### R9 — P2: The memory assertion excludes retained staging memory and graph churn

**Evidence:** `cluster_meta_store.cpp:587–598` omits `pending_` capacity, its dynamic fields, and hash-map overhead. `CommitStaged` calls `pending_.clear()`, which retains the vector allocation. The footprint test stages 20,000 graphs in one batch and then measures this partial estimate; it does not test 100,000 graphs. Deleted names/placements remain indexed until reload. Migration records also retain terminal entries unless explicitly pruned.

The compact index is a good design, but the reported bytes per graph are not total resident catalog memory. Large batches leave retained staging allocations, and repeated creation/deletion grows memory with historical names rather than live graphs. New-name insertion also scans the entire pending vector for a matching UID, making a large creation batch quadratic.

**Recommendation:** Account for staging and container capacity, bound batches, replace the pending linear lookup with a transaction-local index, and define tombstone compaction and migration-history retention. Measure warm steady-state and peak RSS/PSS at 100k graphs and under churn, with realistic name lengths and both refcount configurations. The reported nine-process RSS is useful for that small workload, not a loaded-cluster sizing result.

### R10 — P2: State changes do not publish the new shard configuration version

**Evidence:** `SetShardState` writes a bumped `config_version` to disk at `cluster_meta_store.cpp:304`, but `CommitStaged` updates only `state` at line 559. The live descriptor therefore differs from its reloaded form. `CacheInvalidatedByShardChange` masks this by using `RegisterShard` to return online with a new endpoint.

**Recommendation:** Publish the complete resulting shard descriptor, including its version, and test `ONLINE → OFFLINE → ONLINE` using only `SetShardState`. The resolver must refresh leader discovery on the new configuration rather than reuse the previous online period's entry. Health checking on each resolve remains a useful improvement.

## 3. Additional design corrections

- **Rebalancing:** `migration_manager.cpp:324–330` requires every individual move to strictly reduce the global peak. For equal weights and counts `[10, 10, 0]`, it returns no moves because moving one graph leaves the other peak at 10. Use an objective that improves across tied peaks, such as weighted imbalance with a deterministic tie-break, and add this three-shard case. Drain needs a separate policy: `ShardManager::IsHealthy` currently rejects `DRAINING`, which would stop serving existing graphs rather than only stop new placement.
- **On-disk compatibility:** Placement encoding acquired a UID without a schema/version envelope. `Reload` silently skips records rejected by `DecodePlacement`. Before supporting upgrades, either provide an explicit conversion or reject an incompatible catalog with an actionable error. Test old-format and malformed records; never silently turn persisted graphs into missing placements.
- **Catalog epochs:** Restrict `SetVersion` to validated restore/bootstrap usage and reject backwards versions in ordinary operation. A mutable public setter must not invalidate the monotonic epoch contract.
- **Transport integration:** Specify authentication/ACL propagation, bounded forwarding hops, deadlines/cancellation, stale-route refresh, and ambiguous-write retry behavior before implementing a real `Forwarder`. A network error after a write is sent must not trigger an unrestricted automatic replay.

## 4. Decisions on the eight questions in REPORT.md

| Question | Recommendation |
|---|---|
| 1. Explicit staging or commit hook? | Keep explicit ownership, but encapsulate commit/publication in one transaction-owned API as in R1. Unpaired public `CommitStaged()`/`RollbackStaged()` is not sufficient. |
| 2. Warning or hard startup error? | Hard error for unsupported mixed writes or uncovered mutation surfaces. Publish a supported configuration matrix and transition plan. |
| 3. 64-bit or 32-bit UID? | Keep 64 bits. Correct incarnation semantics matter more than a few bytes. Reducing a field to 32 bits does not itself guarantee a 20-byte C++ struct because of alignment. |
| 4. Fence in control or apply path? | Policy may stay in `ClusterControl`; enforcement belongs in the common write admission/replicated apply protocol with durable ownership epochs. See R4. |
| 5. Is kill-based chaos enough? | No for Phase 3 sign-off: PROJECT.md explicitly requires partition safety. Test isolated leaders, asymmetric communication, minority writes, healing, and bounded client timeouts using an authorized network-fault environment. Host sudo is one implementation option, not the acceptance criterion. |
| 6. Bolt HA future? | Converge on one replication engine per shard while retaining Bolt as a protocol. Prefer adapting Bolt into the established common mutation path; keep the second engine experimental/restricted until its full coverage is proven. Do not promise two generally supported engines now. |
| 7. Nine-node E2E gate? | Required for Phase 4 acceptance and before enabling live migration. Small component PRs can merge while disabled and accurately labeled. PROJECT.md requires **30,000 graphs total across the cluster**, not 30,000 per shard; 90,000 is a useful stretch target. Migration design may continue, but data-movement implementation should follow the integrated baseline. |
| 8. Compact refcount default? | Keep opt-in. Validate both layouts for correctness and benchmark optimized throughput, tail latency, and memory with representative concurrency before changing the global default. A compact `-O0` pass cannot settle the false-sharing tradeoff. |

## 5. Recommended delivery sequence and gates

1. **Repair component invariants.** Close R1–R3 and R7–R10 with targeted abort, concurrency, reload, incarnation, and batch tests. Establish one commit/publication abstraction shared by the catalog and migration manager. Add format validation before persisting more test state.
2. **Close the HA acceptance gap.** Enforce R5; repair the durability oracle; exercise all supported write surfaces, snapshot catch-up, partitions, and retries. Reproduce the backup/restore SIGSEGV with symbols, container-local cores, and an ASAN build before assigning its cause. Run a qualifying uninterrupted 24-hour soak; extend to 72 hours according to failure history and release risk. Preserve exact build and run artifacts.
3. **Build one small vertical sharding slice.** Use a replicated catalog, three replica groups, real forwarding, automatic create placement, and receiver fencing. Start with a small graph count. Demonstrate create/read/write/delete/recreate, graph/ACL isolation, router and catalog-leader restart, shard-leader failure, stale direct requests, and bounded retries through one client endpoint. Graph creation should progress from durable intent to data creation to `ACTIVE`, with crash recovery between steps.
4. **Prove Phase 4 at scale.** Run nine data nodes plus the required control/router services, distribute at least 30k graphs, and exercise a bounded active working set. Measure placement balance, admission timeouts, routing latency, memory, file descriptors, failover, and complete per-replica data reconciliation. Use optimized builds for performance conclusions; retain the low-memory build as a correctness configuration. Pin build images and archive logs/checksums.
5. **Then implement migration data movement.** Define snapshot/catch-up watermarks, source revocation, destination activation, retry/deduplication, and when rollback remains safe. Require crash recovery at every boundary and validation before source deletion. Test draining and tied-load rebalancing before enabling automatic moves.

## 6. Reporting corrections

Update REPORT.md and the architecture status documents together after fixes. In particular:

- Describe staging as store-global until R1 is fixed, and migration state as uncommitted in-memory mutation until R2 is fixed.
- Do not claim routing/fencing key on `unique_id` until the request and fence APIs actually carry it.
- Label the current soak evidence as kill/restart recovery and final-counter convergence; identify the exact driver behind the running 24-hour run.
- Label the footprint assertion as a partial index estimate at 20k graphs, with a separate 100k extrapolation and explicit exclusions.
- Reconcile `docs/architecture/12-sharding-status.md`: it still says 14 tests, 16-byte placements, no Phase 5 work, and that nine processes cannot fit, despite newer reported evidence.
- Reconcile the write-path audit's earlier mixed-mode mitigation with its later single-order decision. Preserve the distinction between independent HA groups and an integrated sharded service.
- Attach immutable artifacts to validation claims. Counts such as “34/34” and “404 kills” are useful only with revision, workload, configuration, oracle, and exit status.

The next review should be driven by these invariants and acceptance gates, rather than additional component count or longer runs of an incomplete oracle.
