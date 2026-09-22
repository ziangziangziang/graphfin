---
name: graphfin
description: >
  Use the GraphFin database: store and query graph relationships with
  native time-series observations. Use when asked to connect to GraphFin,
  model entities and relationships with changing measurements, write or
  read series points via Cypher, run the telemetry/financial examples, or
  check GraphFin capabilities and limits. Prefers the local runtime image;
  falls back to building from source.
---

# GraphFin agent skill

GraphFin is a property-graph database with **native time-series fields on
vertices and edges**. Model: traverse relationships to select entities, then
read their changing observations. Investment analytics is the first use
case; the engine is general-purpose.

## Connect

Fastest path — local runtime image (linux/arm64, prebuilt from the qualified
candidate; HTTP 7071, RPC 9091, data in `/data`):

```bash
docker run -d --name graphfin \
  -p 7071:7071 -p 9091:9091 \
  -v graphfin-data:/data \
  graphfin/graphfin:0.1.0-alpha
```

REST (stdlib-only): `POST /login` with `{"user","password"}` → use the
returned `jwt` as `Authorization: Bearer` header, then `POST /cypher` with
`{"script": ..., "graph": ...}`. Dev credentials are `admin` / `73@TuGraph`;
change them before any non-local exposure. RPC and Bolt speak Cypher too
(Bolt listens only when the server is started with an explicit bolt port).
From source instead: follow `docs/getting-started.md` (pinned compile image,
`ci/phase0/build.sh`, then `ci/merge/run.sh smoke` to verify).

## Minimal working sequence

Run each statement separately against a fresh graph:

```cypher
CALL db.createVertexLabel('Sensor', 'id', 'id', 'INT64', false);
CALL db.createSeriesField('Sensor', 'readings',
  [{name:'temperature', type:'DOUBLE'}], {}) YIELD field RETURN field;
CREATE (s:Sensor {id:1});
MATCH (s:Sensor {id:1})
CALL series.append(s, 'readings',
  {ts:datetime('2024-01-02 00:00:00'), temperature:21.5})
YIELD written RETURN written;
MATCH (s:Sensor {id:1}) RETURN series.latest(s, 'readings') AS latest;
```

Runnable end-to-end variants: `demo/SeriesTelemetry/telemetry.py` and
`demo/SeriesFinancial/financial.py` (both take `--port`).

## Operation catalog

- DDL: `db.createSeriesField` / `db.createEdgeSeriesField` (measures +
  `{bucket_max_points, bucket_max_span_us, bucket_max_bytes}`),
  `db.dropSeriesField`. Appending `CALL ... YIELD ... RETURN ...` is a
  replicated write like any other — never skip the `RETURN` shape in tests.
- Writes: `series.append` (replaces the point at the timestamp),
  `series.update`, `series.update_cas` (value-based compare-and-set),
  `series.clear`. In-query `MATCH ... CALL ... YIELD ... RETURN ...` forms
  commit atomically per query.
- Reads: `series.at` (miss → `null`, not error), `series.range`
  (inclusive bounds), `series.latest` / `series.earliest`,
  `series.count`, aggregates (`series.mean/sum/min/max`).
- Discovery: `CALL db.vertexLabels()`; per-label series metadata comes from
  one summary map (`RETURN n.<field>`), which lists `measures`.

## Wire rules that bite

- REST and Bolt return collection cells (points, ranges, summaries) as **JSON
  text** — always `json.loads` them; never infer type from content. RPC JSON
  arrives pre-parsed. Empty range is `"[]"`/`[]`, never null.
- `INT64` is exact end-to-end; JavaScript consumers must stringify values
  outside ±2^53. Timestamps are **microseconds, no timezone**.
- New non-finite `DOUBLE` writes are rejected. Append replaces; an omitted
  measure reads back `null`. `update_cas` returns `written: 0` on stale
  expectation instead of overwriting.
- Retry only: identical bytes, same writer, nothing interleaved (dropped
  connection/timeout). Never auto-retry semantic errors, `clear`, or DDL.
  Concurrent writers must use `update_cas`.

## HA and hard boundaries

- 3-node clusters: send writes to the leader (`callCypherToLeader` on RPC);
  a follower answers reads from its local state, which may lag. Writes
  acknowledged before leader loss survive; in-flight, partition, and soak
  behavior is qualification-gated per `docs/testing/post-merge.md`.
- One mutable point per timestamp — no revision history, no "known-then"
  queries. Transactions span **one graph**. GQL series syntax is partial;
  write series only in Cypher.
- No distributed forwarding, online migration, revision queries, or native
  batch ingestion. No supported downgrade or mixed-version HA.

## Pointers (repo root)

`README.md` (framing + 30-second example) · `docs/product.md` (boundaries) ·
`docs/getting-started.md` (build/run) ·
`docs/architecture/09-series-client-contracts.md` (full wire contract) ·
`docs/testing/post-merge.md` (what is/isn't qualified) · `RELEASE.md`
(release status). Product `0.1.0-alpha`, engine baseline TuGraph `4.5.2`.
