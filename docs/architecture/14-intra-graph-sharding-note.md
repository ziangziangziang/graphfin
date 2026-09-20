# 14 — Intra-Graph Sharding (Phase 7): Not Started

## Status

**Deferred (a separate distributed-database program).** Phase 7 is explicitly
out of scope until Phases 3–6 are accepted. This note records why and what it
entails, so the whole-graph architecture is not compromised in the meantime.

## Why it is deferred

Whole-graph sharding (Phase 4) keeps a graph on exactly one shard, so query
execution never crosses a shard boundary. Intra-graph sharding removes that
invariant: vertices, edges, indexes and queries span machines, which requires:

- a partitioning model (vertex→partition, edge ownership) and partition
  metadata;
- cross-shard reads and **distributed traversal** with remote expansion,
  result aggregation, cancellation, timeouts, backpressure;
- cross-shard writes with defined failure behavior;
- **distributed transactions** (coordinator, prepare/commit, recovery logs,
  timeouts, participant recovery, deadlock strategy);
- online repartitioning and automatic balancing.

Each of these is a research/engineering effort comparable to the whole of
Phases 4–6 again.

## Guardrails

- Do not weaken the Phase 0–6 single-shard guarantee (no cross-shard execution).
- Keep the fast path: a query confined to one partition must not pay
  distributed-execution costs.
- Build extensive correctness tests before any performance work.

## Entry criteria (before starting Phase 7)

All Phase 4 success criteria met (live 3-shard, 30k graphs, receiver-side
fencing, replicated control plane), Phase 5 migration reliable, and Phase 6
SLOs/runbooks in place.
