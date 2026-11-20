# 09 — Series Client Contracts

Wire shapes, value rules, error catalog, retry guidance, and parser support
for time-series query results. Every claim below was verified against a live
server (`lgraph_server` from `qa/local/run.sh build`) through real clients
(REST over HTTP, Bolt via the `neo4j==4.4.6` driver package, and the bundled
`TuGraphRestClient`); in-process golden values alone do not establish these.
Cells were read with `series.at/range/latest/earliest`, summaries via
`RETURN n.<field>`, and scalars via `series.count/mean`.

Verification environment: arm64 CentOS 7 container, `timeseries` branch,
default configuration.

---

## 1. Collection normalization

`LIST`/`MAP` result cells cross every transport as JSON **text**:

| Transport | Point / summary / range cell | Empty range |
|---|---|---|
| REST `/cypher` | JSON string, e.g. `"{\"close\":12.5,...}"` | `"[]"` (string) |
| Bolt (packstream) | JSON string | `"[]"` (string) |
| RPC JSON | Structured object (the RPC JSON layer re-parses the text) | `[]` |

Consequences:

- A REST/Bolt client **must** `json.loads` collection cells. Never guess
  from content: a `STRING` measure holding `'{"a":1}'` arrives as the same
  JSON-looking text and must stay a string. The server never parses cell
  text back; only the RPC JSON convenience layer does.
- SDK normalization proposal (not yet implemented in any bundled SDK):
  clients should expose `result` (raw, as above) plus a `parsed()` view
  for collection columns. This needs an M4 wire/API design decision first:
  the current header carries type 0 for scalar, LIST, MAP, and ANY result
  types alike (`state_machine.cpp`), so the header as shipped cannot tell a
  collection from a JSON-looking string. Options: compatible logical-type
  or value-tag metadata (including dynamic ANY expressions), or an explicit
  caller-supplied result schema. Content sniffing is not acceptable, and the
  ANY-cell serialization repair does not supply this distinction.
- `RETURN []` / `RETURN {}` arrive as the strings `"[]"` / `"{}"`, never
  null. An empty window is `[]` after parsing.

## 2. INT64, precision, and timezone rules

- `INT64` measures and `series.count` cross exactly on every transport,
  including `9223372036854775807` and `-9223372036854775808`. Python `json`
  preserves them; JavaScript consumers must treat values outside
  ±2^53 as strings — no silent precision loss is acceptable, so document
  string transport for those clients when an SDK targets them.
- Timestamps are **microseconds, no timezone**. `datetime('2024-01-02
  00:00:00')` means local wall time as written; REST renders it back as
  `"YYYY-MM-DD HH:MM:SS"`, Bolt as a `LocalDateTime` struct
  (`'d'`, `[seconds, nanos]`). Fractional seconds keep microsecond
  precision end-to-end. Stored timestamps must lie in the DATETIME domain;
  out-of-range integers are rejected at write time (a successful write
  never breaks later reads).
- `DOUBLE` measures keep IEEE semantics through JSON (17 significant
  digits) and Bolt float64. Integer `mean()` accumulates in double;
  integer `sum()` is overflow-checked and refuses rather than wraps.
  Non-finite values (NaN, ±Infinity) are rejected at every write boundary
  with a clear error: they serialize as JSON null while staying non-null
  in the engine, which would silently conflate values with missing
  observations. Previously stored non-finite bytes still decode (codec
  round-trips them bit-exactly); only new writes are refused.

## 3. Null / empty / nesting matrix

| Expression | REST cell | RPC cell | Notes |
|---|---|---|---|
| `series.at` hit | JSON-text map | object | `{"close":12.5,"ts":"...","volume":7}` |
| `series.at` miss | `null` | `null` | No such timestamp: null, not error |
| `series.range` empty window | `"[]"` | `[]` | Never null |
| Missing measure in `append` | `null` value in map | `null` | Full-point replacement contract |
| Explicit null measure write | `null` value in map | `null` | Same as missing |
| `RETURN null` | `null` | `null` | Empty Bolt `std::any`, never `0`/`""` |
| `RETURN []` / `{}` | `"[]"` / `"{}"` | `[]` / `{}` | Shape preserved, never null |
| Nested list/map in results | JSON text, structure kept | structured | Empty containers stay `[]`/`{}` |
| `'{"a":1}'` string property | `'{"a":1}'` string | `'{"a":1}'` string | Type by declaration, not content |
| `c.prices` on empty series | `{"count":0,...}` text | object | `first`/`last` null; measures listed |

## 4. Capability discovery, errors, retry

Discovery today: `CALL db.vertexLabels()` lists labels; a label's series
metadata (measure names, bounds) is discovered by reading one summary map
(`RETURN c.prices`), whose `measures` array names every measure. There is
no dedicated series-catalog procedure yet.

Documented series errors (stable text, asserted by goldens and REST tests):

| Trigger | Error |
|---|---|
| LIST/MAP measure value (`[99]`, `{v:99}`) | `<proc>: value for measure '<m>' must match its declared type` |
| Out-of-range option (`bucket_max_points:4294967297`, `-1`, unknown key) | `<proc>: bucket_max_points must be in [1, 1000000]` / unknown-option text |
| Out-of-DATETIME-range timestamp | `<proc>: timestamp out of DATETIME range` |
| Unknown measure in `series.update` | `Series field [...] has no measure [...]` |
| Ordinary `SET` on a series field | names `series.append` / `series.update` |
| Reload-shifting DDL on packed labels | names the shifting field, suggests fast-alter or drop-first |

Retry guidance (R7): consecutive identical writes are idempotent, but replay
is NOT safe across intervening writes — replaying request A after correction
B silently restores A's value, and replaying a clear erases newly arrived
points. Automatic retry is therefore restricted to an **ordered exclusive
writer** (same bytes rewritten, nothing interleaves): transport-level
failures (dropped connection, timeouts) may be retried there; semantic
`CypherError`s are never retried; `clear` and DDL are never auto-retried.
`test_timeseries.py::idempotent_retry` enforces this split (refuses
non-retryable ops outright). Concurrent mutable ingestion must use
`series.update_cas` with expectations: a stale replay loses explicitly
(`written=0`) instead of undoing the correction. Provisional until the M5
revision model lands.
Multi-row `MATCH`+`CALL` writes commit atomically with the query; a later
row's type error rolls back earlier rows of the same query.

## 5. Batched append (`series.append_batch`)

`CALL series.append_batch(element, field, points) YIELD written, replaced`
(underscore alias `series_append_batch`) writes many points to one vertex's
or edge's series field in a single pass. `points` is a list of the same
`{ts, measure...}` maps `series.append` takes, with the same per-point
rules: missing measures become null (full-point replacement), `ts` is a
DATETIME or INT64 microseconds in the DATETIME domain, DOUBLE values must
be finite and match the declared type, unknown measures are rejected. At
most **100000 points per call**; chunk larger ingestions.

Semantics:

- **Order-independent layout.** The batch is sorted internally, so shuffled
  or descending input produces the same bucket boundaries as ascending input.
  Unlike repeated `series.append`, a descending back-fill does not fragment
  into one bucket per point. (Which value survives a repeated timestamp is
  input-order dependent — see last-wins below.)
- **Last-wins duplicates.** Equal timestamps within one batch collapse to
  the last occurrence in input order.
- **Stats.** `written` counts distinct timestamps new to the series,
  `replaced` counts corrections of stored points; `written + replaced`
  equals the number of distinct timestamps in the batch. An empty list
  yields `0, 0` and writes nothing.
- **Atomic.** Every point is validated before anything is written: one bad
  point anywhere rejects the whole call with nothing stored. A byte-cap
  failure aborts the query's transaction like any other write error.
- **Layout.** When the bucket point cap binds, splits happen at exactly the
  same moments as sequential ascending `series.append` calls, so the stored
  bytes are identical to those calls when both resolve repeated timestamps to
  the same last-wins winner (unit golden). When only the byte cap binds, the
  point set is the same but bucket boundaries may differ.

Cost: each touched bucket is decoded and re-encoded once per call instead
of once per point. The recommended ingestion shape is one batch call per
element per transaction, driven by a multi-row `MATCH` (e.g.
`MATCH (s:Symbol) WHERE s.id IN [...] CALL series.append_batch(s, ...) YIELD ...`);
`UNWIND`-leading single-part queries are not supported by the engine. A
workload that emits one point per second per symbol should micro-batch per
symbol (e.g. a few seconds of points per call) rather than call per point.
Retry guidance is the same as for point writes: an exact replay is
byte-idempotent under an ordered exclusive writer; concurrent mutable
ingestion still needs `series.update_cas`.

Additional stable error texts (same catalog rules as §4):

| Trigger | Error |
|---|---|
| Point map without `ts` | `series.append_batch: point map needs a 'ts' entry` |
| Non-map list item | `each point must be a map of ts plus measure values` |
| More than 100000 points | `series.append_batch accepts at most 100000 points per call` |
| Unknown measure / bad value / bad timestamp | same texts as `series.append`, prefixed `series.append_batch` |

## 6. Parser support

| Capability | Cypher (Lcypher) | GQL (GEAX path) |
|---|---|---|
| `series.range/at/latest/earliest/count/mean/...` reads | Full | Dotted names do not lex; `series_range` etc. underscore aliases work |
| `series.append/append_batch/update/clear` writes | Full | Map literals arrive as unsupported `MkRecord`; writes stay Cypher-only |
| `db.createSeriesField` / `db.dropSeriesField` | Full | Scalar-argument DDL works; map-argument DDL does not |
| Inline label-DDL series sugar | Deferred on both | Deferred on both |

GQL limits are pinned, not fixed: unsupported syntax must surface a stable
error, and clients must not imply GQL writes work.

## 7. Examples

`demo/SeriesTelemetry` (sensors: temp/humidity/flags) and
`demo/SeriesFinancial` (symbols: OHLCV) run declare → ingest → query
(range/latest/mean) → JSONL export against a live server using stdlib-only
REST. Sector/site fields are ordinary indexed properties; nothing in the
engine is fixture-specific.
