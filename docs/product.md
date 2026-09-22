# Relationships and observations, together

GraphFin stores labeled vertices and edges with native time-series fields.
Applications can follow a dependency and read the observations attached to the
selected entities or relationships. Each named graph has its own transactional
store; lazy loading and bounded admission keep inactive graphs from requiring
all their runtime resources at once.

![Connected graph entities above sampled time-series layers](images/product/graph-time-hero.png)

This is a conceptual product illustration, not a deployed-cluster diagram.

## Capability boundaries after the merge

| Area | Implemented | Qualification / remaining work |
| --- | --- | --- |
| Property graph | Labeled nodes/edges, graph queries, indexes and ordinary properties inherited from TuGraph | Existing API limitations still apply |
| Native series | Vertex/edge fields; typed measures; append, update, CAS, clear, exact-point/range/endpoint reads and aggregates | One mutable observation per timestamp; no built-in revision history |
| Transactions | Series buckets live in the graph's LMDB transaction | Cross-graph transactions and a global multi-graph snapshot are not promised |
| Multi-graph resource control | Lazy graph open, eviction, cap/admission and lifecycle metrics | Active operations pin resources; graph-count limits are not complete process-RAM limits |
| Replication | Existing three-node HA machinery and parent-branch chaos harness | Series schema/values must pass merged-binary failover and reconciliation tests; choose one write-replication path |
| Whole-graph sharding | Persistent placement catalog, immutable graph identity, routing decisions, fencing and migration state components | Live forwarding, server-side fence integration, replicated catalog, admin surface and migration data movement are unfinished |
| Clients | REST/RPC/Bolt paths and Python examples | Collection normalization, error behavior and required driver compatibility have explicit contracts |
| Operations | Restart, backup and snapshot tools | Qualification scope depends on evidence for the exact merged build |

## Example applications

A financial application models issuers, suppliers, products, dealers and holdings,
then attaches price, volume, fundamentals or ownership observations. A telemetry
application models machines, sensors and upstream dependencies with sampled
measurements. Both use the same generic series and graph APIs. Entity resolution,
market feeds, causal/statistical models and trading decisions belong above the
database.

Historical questions such as “what was known at the time?” require future
revision and graph-temporal semantics. A timestamped series and an ordinary edge
do not by themselves prevent look-ahead bias or encode historical ownership.

## Data contracts

- Timestamps use microsecond DATETIME precision without a timezone identifier.
  Applications must choose and document a common time basis.
- Append replaces the point at an existing timestamp; missing measures become
  null. Update changes a selected measure.
- Value-based CAS detects a mismatched current value. It is not a persisted
  request-deduplication or revision-token API and does not solve ABA changes.
- New non-null DOUBLE writes must be finite. Existing non-finite stored values
  still need an explicit export/migration policy.
- INT64 scaled values are an option when an application needs exact fixed-scale
  quantities; this is an application convention, not a financial core type.
- Range bounds are inclusive. Current range materialization is not a bounded
  streaming or stable-cursor API.

See [the wire contract](architecture/09-series-client-contracts.md) and
[qualification plan](testing/post-merge.md) for precise limits and tests.
