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
