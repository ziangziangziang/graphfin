# Relationships and observations, together

Many graph problems are also time-series problems. Real-world data is both
**connected** and **changing**: a company is connected to suppliers,
subsidiaries, securities, funds, industries, and counterparties, while the
facts attached to those entities and relationships — prices, holdings,
fundamentals, exposures, volumes, ownership stakes — evolve continuously.

Traditionally those two dimensions live in different systems. The graph
database knows **what is connected**. The time-series system knows **what
changed and when**. The application is left to align identities, coordinate
reads and writes, and move between the two models.

GraphFin lets vertices and edges own native time-series fields, so
applications traverse the relationships that matter and work with their
changing observations through the same data model.

The graph provides the **context**.
The series provides the **history**.
The relationships tell you **where to look**.
The series tell you **what happened over time**.

It is built for connected, time-varying data — with **investment analytics
as its first major use case**, while keeping the underlying database
general-purpose. Each named graph has its own transactional store; lazy
loading and bounded admission keep inactive graphs from requiring all their
runtime resources at once.

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

## Why this shape

**One identity.** A series belongs to a vertex or edge in a named graph. The
application does not need a second store's identifier just to find that
element's observations.

**One transaction domain.** Graph records and series buckets live in the same
graph store. Changes made within one graph transaction commit or roll back
together. Separate requests still create separate transactions.

**Graph-first selection.** Follow a holding, supplier, or dependency
relationship to find the entities that matter, then read their observations.
The graph narrows the question before the series answers it.

**Native storage.** Observations are stored in timestamp-ordered, encoded
buckets, with typed measures and point/range operations. Applications can
correct a measure or read a window without treating an entire history as an
ordinary property value.

## Example applications

**Finance.** Model issuers, securities, funds, suppliers, and counterparties.
Attach prices and fundamentals to entities, and positions or weights to
relationships. Use the graph to select exposures and the series to examine
how their observations changed. A security vertex can own its price series; a
company can carry its fundamentals; a `HOLDS` edge can carry a position
series.

**Telemetry.** Model machines, sensors, sites, and upstream dependencies.
Attach temperature, throughput, or health measurements to the equipment and
relationships they describe. Follow a dependency to find the relevant
readings.

**Dependency systems.** Model services, components, supply chains, or
infrastructure. Track latency, volume, capacity, or lead time alongside the
connections that give those measurements meaning.

All three use the same generic series and graph APIs. The database provides
the relationships and observations. Applications provide entity resolution,
feeds, domain models, and statistical or causal analysis.

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
