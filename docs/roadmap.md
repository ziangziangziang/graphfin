# Combined roadmap

The repository merges two work streams: multi-graph performance/distribution and
native time-series storage. Neither parent branch's plan alone describes delivery
of the combined product. This document coordinates them; historical PROJECT.md,
PLAN.md, REPORT.md and REVIEW.md remain evidence of their original work streams.

The direction is stronger operational qualification, stable series identity,
batched ingestion and bounded reads, integrated whole-graph sharding, and
historical analysis primitives — in that order where contracts force an order.

| Order | Deliverable | Exit criterion |
| --- | --- | --- |
| 1 | Merged release baseline | One identified merged build passes series, cluster and applicable lifecycle/client gates; failures and unsupported capabilities published |
| 2 | Stable series identity and metadata | Non-reused persisted identity survives scalar schema changes and reopen; reviewed collision-safe migration and downgrade policy |
| 3 | Useful mutable-series APIs | Batched/resumable ingestion, explicit duplicate policy, bounded reads/as-of access and streaming aggregates; paired finance/telemetry tests |
| 4 | Operational qualification | Active-reader pressure, in-flight transaction crashes, real backup/snapshot restore, full sanitizers, released-fixture compatibility and HA series reconciliation |
| 5 | Integrated whole-graph sharding | Real forwarding and receiver fencing, replicated placement authority and admin APIs; 3 shards × 3 replicas, 30k graphs through the public logical endpoint |
| 6 | Historical analysis primitives | Observation revisions, validity/knowledge-time graph selection, aligned windows and graph-selected multi-series computation |
| 7 | External analytics and lifecycle | Export and committed change delivery, lineage, retention, rollups and reproducible derived results |
| 8 | Migration and production scaling | Reviewed byte movement/catch-up/cutover with fencing and interruption recovery; measured workload budgets and completed soak |

Rows can proceed concurrently where contracts allow. Mutable batch work and
bounded reads need not wait for the whole historical model. Persisted history and
migration formats need identity/compatibility review first. Keep current DDL guards
until a safe series-identity migration exists.

Intra-graph sharding, cross-shard query execution and distributed graph transactions
are separate designs, not consequences of whole-graph placement. Financial models
and vendor-specific ingestion are application projects, not engine milestones.

## Reorganization track (REVIEW.md follow-ups)

Merged in PR #1 (`d8d1b0ed`); verified present in `main`. The pre-merge
reorganization review is archived at
`docs/history/review-reorganization-archived-2026-09-27.md` and is historical
evidence, not a current checklist.

| Status | Item |
| --- | --- |
| done | PR fast checks vs main full suite |
| done | `ci/phase0/` → `dev/phase0/` |
| done | `graphfin_server` alias, `lgraph_server` compat |
| done | `test/suites.json` catalog; merge/phase0 runners wired |
| done | `pr-unit` consumed by a workflow job (`pr_engine_gate`); case-count, failure, skip and required-artifact gate |
| done | Server alias smoke check wired into the PR path |
| TODO | Mark `pr_engine_gate` required in branch protection (repository setting, not code) |
| TODO | Confirm the 45-minute cold-seed budget from a real first run |
| done | `dev/phase0/experiments/` probes relocated to `test/stress/`; script paths and doc references updated |
| TODO | Scope CodeQL c-cpp off PRs (settings change, not code) |
| TODO | GraphFin-tagged images + new lockfile; client/lib/storage migration per REVIEW stages |

## Database foundation track (REVIEW.md DB-01…DB-12)

Source-level findings from the 2026-09-27 review. None is a reproduced defect;
each needs a test that fails before the fix. Ordered by the review's priority.

| ID | Status | Item |
| --- | --- | --- |
| DB-10 | in progress | Required pre-merge engine gate. Workflow wiring and result gate landed; branch protection pending |
| DB-01 | TODO | One authoritative mutation order. Legacy HA + Bolt HA together is only warned about at `src/server/lgraph_server.cpp:134`, not rejected. Needs coverage across RPC/REST/Bolt/schema/ACL/lifecycle/import |
| DB-02 | TODO | Crash-safe replay markers. `src/server/ha_state_machine.cpp:372` applies before publishing the index; `ApplyRaftRequest` in `src/server/bolt_handler.cpp` updates its index separately and catches execution exceptions. Needs fault injection around commit, marker publication and response loss |
| DB-02 | TODO | Deterministic replay. `BuiltinFunction::Rand` uses local randomness, so replicated statements can diverge. Audit random/time functions, generated identities and plugin side effects |
| DB-03 | TODO | Explicit durability/read/retry contracts. `MDB_NOSYNC` is unconditional at `src/core/lmdb_store.cpp:71`; `durable` defaults false at `src/core/data_type.h:156`; no visible quorum read barrier in `HaStateMachine::DoRequest` |
| DB-04 | TODO | Bounded work and tenant fairness. `SeriesStore::Range` (`src/core/series_store.cpp:601`) materializes every selected point into a caller-supplied vector, and `SeriesAggregate` (`src/cypher/arithmetic/arithmetic_expression.cpp:1656`) always calls that range even for scalar mean/min/max/sum. Needs streaming aggregates, result budgets and cancellation checkpoints |
| DB-05 | TODO | Efficient ingestion and measured hot-graph capacity. `SeriesStore::Upsert` (`src/core/series_store.cpp:538`) decodes and rewrites a bucket per point; needs native batches with duplicate policy and resumability |
| DB-06 | TODO | Integrated horizontal scaling. Routing/fencing/placement exist as components; live forwarding, receiver fencing and interrupted-migration safety are unfinished (`docs/architecture/12-sharding-status.md`) |
| DB-07 | TODO | Stable series identity independent of packed field positions; on-disk format versions and released-fixture compatibility |
| DB-08 | TODO | Recoverability as an operated capability: restore drills, RPO/RTO per failure class, verified (not merely successful) backups |
| DB-09 | TODO | Client contracts and tenant security: typed versioned results, exact integers, microsecond timestamps, stable error categories; ACL coverage for series and DDL paths |
| DB-11 | TODO | Independent observability. Native analytics must not replace external scraping and alerting |
| DB-12 | TODO | Temporal analysis: as-of reads, revisions, event/knowledge time, historical relationship validity |

## Management console track (REVIEW.md §7–§12)

The console is still the inherited TuGraph Browser. Discovery first; no
rebranding or rewrite before the pinned UI is reproducible and audited.

| Status | Item |
| --- | --- |
| TODO | Audit the pinned console. `deps/tugraph-db-browser` and `deps/tugraph-web` are uninitialized; `src/restful/server/browser-resource` is absent, so `src/CMakeLists.txt` cannot complete a full install and `release/local/etc/lgraph.json` points at a path this checkout cannot produce |
| TODO | Decide console ownership: audited upstream reuse vs a new `console/` shell |
| TODO | Backend contracts before UI: series schema catalog, bounded large-window reads, conditional update, health semantics, placement visibility, recovery jobs |
| TODO | Select and qualify a browser transport route into the chosen mutation order; no independent REST/Bolt write paths |

## Queued product work

| Status | Item |
| --- | --- |
| TODO | Replace Prometheus monitoring with native GraphFin time-series monitoring |
| TODO | Recreate demo with real GraphFin structures + metrics demo; remove legacy demo |
| TODO | Release Python clients |
| TODO | Site hero 3D scene (`site/src/scene/`) — landing-page storytelling only; keep it out of the console and out of authenticated administration |
