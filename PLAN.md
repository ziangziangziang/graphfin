# GraphFin build plan in response to PROJECT.md (M0–M8)

> Time-series work-stream plan. The merged PROJECT.md retains the performance
> branch's phases, so its headings are not the M0–M8 references below. Use the
> [combined roadmap](docs/roadmap.md) and [post-merge tests](docs/testing/post-merge.md)
> to coordinate both streams; retain these detailed series design inputs.

> **Current merged-release status (2026-09-22):** GraphFin `0.1.0-alpha` is in
> qualification. The strict unit gate is 109/109, the joint smoke gate is 4/4,
> and the client gate is 14/14 with `neo4j==4.4.6`. The latest HA series gate
> failed both cases, so the project is not signed off. Use [TASK.md](TASK.md) to
> delegate the remaining diagnosis, packaging, publication, and review work.

## Guiding rules

- Production claims require M0 first; API names, disk formats, and query syntax
  need a design + compatibility review **before** implementation.
- Each increment lands independently reviewable with attributable evidence
  (commit + source/binary hashes + test results, per the series gate).
- Compatibility constraints (PROJECT.md §Scope) are checked every phase:
  single-point replacement, inclusive bounds, microsecond DATETIME, one
  point per timestamp, documented txn atomicity, legacy wire behavior
  (LIST/MAP as JSON text), warning-clean builds.
- Every semantic feature gets a financial AND a non-financial fixture on the
  same code path. Benchmarks never gate unit tests on wall-clock time.

## Phase 0 — Close M0 production evidence (gates everything)

Prerequisites: none (runs alongside later design work). Exit: M0 success
criteria signed.

| # | Work | Acceptance |
|---|---|---|
| 0.1 | Integrated sanitizer runs: fix image/toolchain so full-binary TSan (needs OpenMP-free vsag or newer GCC) and ASan (needs RTTI-complete rocksdb) run; integrate into gate | Zero findings; TSan covers readers, R/W overlap, independent + same-series writers; ASan/UBSan covers decoder fuzz + lifecycle tests |
| 0.2 | Eviction/reopen under constrained memory with active readers + pending work (config-capped cache, lifetime asserts) | Data preserved; no use-after-free; iterators/txns/stores outlive correctly |
| 0.3 | Abrupt-termination recovery under durable mode (SIGKILL mid-write, committed vs uncommitted) + real snapshot/backup-tool restore | No acknowledged durable commit lost; no partial txn exposed; tool restore reproduces schema + values |
| 0.4 | Genuine v4.5.2-binary fixture; upgrade/reopen test; documented downgrade/mixed-version policy | Fixture loads; upgrade path green; policy written |
| 0.5 | Live Bolt driver assertions via a real driver package (replace/augment TCP driver); REST suite kept | Compatibility matrix passes for nested/empty/temporal/null/INT64-boundary cases |
| 0.6 | HA replication + failover evidence on supported config (replay, ordering, deterministic revision IDs) | Failover preserves committed series; no duplicates on replay |
| 0.7 | Benchmark harness + baselines for all six profiles (seed, hardware, durability mode, hashes recorded); budgets stay open, no invented capacity | Reproducible baselines published; optimization PRs pin deltas against them |

0.1's first step is an image fix outside this stream — escalate early.

## Phase 1 — M1 identity, metadata, schema evolution (S)

Prerequisites: M0 running (not necessarily signed). Foundation for every
persisted-history format — before M2/M5.

### Design inputs for principal review (persisted identity)

Status quo: bucket keys carry the packed-layout positional field id; reload
regroups fixed-before-variable, so any layout edit shifting a series id
orphans buckets. Current guards reject such DDL (retain until migration
ships, per PROJECT.md).

- **Option A — name-keyed buckets.** Put the field name in the bucket key.
  Immune to id shifts; keys self-describing. Costs: variable-length keys
  (still far under 480B); renames become data migrations; one-time re-key
  of existing positional-keyed databases.
- **Option B — stable series ids (recommended).** Persist a per-label
  monotonic series id in `FieldSpec` (+ schema block); key buckets by it.
  Fixed-size keys (current performance story preserved); renames free;
  survives every layout op; the id is exactly the identity the metadata
  APIs must expose. Costs: schema format extension + compat rules; a
  persisted per-label allocator; one-time id assignment for existing DBs
  (safe only from canonical layout states — non-canonical DBs canonicalize
  first, else reject with a clear error).
- **Migration (both options):** re-key inside an upgrade transaction or an
  offline upgrade tool; two-phase write-verify-delete so an interrupted
  migration resumes safely or leaves the old format readable. Downgrade
  across the format change is unsupported (document; refuse with a clear
  error rather than misreading).
- **Open questions:** id width/scope (per-label u16 vs wider), allocator
  placement, detached-property interplay, measure-identity representation
  for the evolution rules.

1. Design review: persisted series identity (name-keyed keys vs. stable IDs +
   mapping table), migration record format, measure-identity/evolution rules.
   Compatibility review required (disk format!).
2. Implement identity + keep/extend DDL guards during transition; versioned
   migration with interrupted-migration recovery; drop/recreate isolation (new
   field can't see old buckets); metadata APIs (identity, types, units,
   timestamp basis, policy) with round-trip.
3. Modeling examples (assertion vertices, aliases, documents) as docs +
   fixtures.
4. Acceptance: add/remove scalar field → reopen preserves identity in
   packed/fast-alter/attached/detached layouts; incompatible measure change
   fails clean; assertion fixture survives export/import.

## Phase 2 — M2 batch ingestion (M)

Prerequisites: M1 identity contract (mutable-series path may start earlier).

1. Design review: batch API shape, duplicate policies, chunking/checkpoint
   semantics.
2. Transaction-scoped batch upsert (validate-then-mutate, sorted packing
   without per-point re-encode); CSV/JSON streaming import (chunk atomicity,
   progress, rejection reports, cancellation, restart checkpoints); 10M-point
   profile run.
3. Acceptance: sorted/shuffled/descending/chunked inputs logically identical
   under policy; malformed tail record rolls back its chunk only;
   retry-after-lost-ack adds nothing; vertex/edge batches share txn rollback;
   memory bounded; bytes/point + occupancy + rewritten-bytes recorded.

## Phase 3 — M3 bounded reads (M)

Prerequisites: series store (have it); synergizes with M1 metadata.

1. Design review: cursor protocol (snapshot, expiry, limits), as-of
   semantics, aggregate streaming API.
2. Selected-measure reads, cursor/iterator paging, last-at-or-before lookup,
   bucket-visitor aggregates with shared decoding, transactional
   count/first/last metadata (with migration rebuild), query diagnostics
   (buckets visited, bytes decoded, cancellation outcomes).
3. Acceptance: as-of fixtures (09:30:00/02/03 cases), page-concatenation ==
   reference scan under concurrent writes, aggregates match reference incl.
   nulls/overflow, narrow-lookup decode counts flat, memory bounded by
   page/bucket state.

## Phase 4 — M4 public APIs + clients (M)

Prerequisites: M2/M3 (extend again for M5/M6 later).

1. Public C++/Python API surface on public headers only; typed SDK helpers;
   capability discovery; error/retry guidance.
2. REST login fix; INT64-beyond-JS-range, precision/timezone, null/nesting
   rules; collection normalization in SDKs; native containers only via
   explicit compat path.
   Design dependency (R10): result headers carry type 0 for scalar, LIST,
   MAP, and ANY alike, so automatic collection normalization needs
   compatible logical-type/value-tag metadata or a caller-supplied schema
   first. No content sniffing.
3. Telemetry + financial executable examples; parser-support matrix
   (GQL limits discoverable).
4. Acceptance: examples use public APIs only; same logical values across
   clients; fresh-client journey (auth → discover → ingest → page → stable
   error) green; legacy shapes retained.

## Phase 5 — M5 versioned history (L)

Prerequisites: M1 (hard), M3 primitives.

1. Design review (biggest one): bitemporal model (`valid_at`/`known_at`),
   revision identity + commit order, corrections/retractions/deletion,
   tombstones across delete/migrate/backup/replicate.
2. Opt-in history storage; temporal context applied uniformly to
   vertices/edges/facts/series in a query; indexes benchmarked before layout
   choice.
3. Acceptance: K1/K2, acquisition-date, retraction, sensor-recalibration
   fixtures — all with restarted-DB persistence, financial and
   non-financial variants.

## Phase 6 — M6 alignment + computation (M)

Prerequisites: M3/M5. Graph-selected batched fetch with pushdown; alignment
policies (exact/as-of-with-age/grid/sessions); window primitives
(lag/diff/stats/grouped agg) with defined null/edge behavior; shared scans;
completeness metadata. Acceptance per plan fixtures (grid alignment, lagged
correlation sign convention, cutoff-weighted holdings,
reference-implementation agreement, decode-count bounds).

## Phase 7 — M7 export/lineage/change delivery (M)

Prerequisites: M1/M5 + M2/M4. Columnar export format (design review, no
library coupling); manifest/lineage conventions; atomic publication;
resumable at-least-once change stream (ordering, retention, backpressure, lag
metrics); invalidation protocol. Acceptance: worker round-trip idempotent; no
aborted txn in stream; disconnect/replay safe; late-correction invalidation;
export-then-publish failure leaves nothing falsely complete.

## Phase 8 — M8 retention/rollups (M)

Prerequisites: M3/M5/M7. Retention/downsampling policies with dry-run +
bounded cleanup; correction-aware rollups; latest-value projections with
freshness tracking; transactional or explicitly-lagging maintenance.
Acceptance: 1s→1min rollup boundaries, expiry semantics explicit,
projections match reference scans across mutations, clean
shutdown/eviction behavior.

## Cross-cutting

- Testing pyramid per phase: unit/property (hand-computed + randomized
  reference) → integration (real clients, crash recovery, tools, workers) →
  benchmarks.
- Risks: (1) image/toolchain fixes for sanitizers are external — escalate in
  Phase 0; (2) M5 design review is the highest-risk review — schedule during
  Phase 3; (3) Bolt driver packaging and v4.5.2 binary sourcing are
  procurement risks — start sourcing in Phase 0; (4) isolate benchmark runs
  from dev test runs.

## Status ledger

Historical series work completed on `timeseries` and merged into the current
GraphFin candidate:

- R1–R6 correctness set: positional-identity DDL guards, fast-alter
  no-default invariant + load recovery, atomic decode counter, LIST/MAP
  rejection, bucket-option validation, DATETIME-domain timestamps.
- Decoder allocation cap + adversarial fuzz battery (gate).
- TSan harness over shared store traffic: clean. ASan/UBSan 20k-mutation
  fuzz: clean. (Full-binary runs blocked — see below.)
- Upstream defects found by sanitizers and fixed: unlocked
  `ThreadIdAssigner` release, misaligned LMDB version loads, ANY-header
  collection-cell server abort.
- S5 matrix: 8 distinct writers, same-series contention with abort-rate
  logging, reader/writer overlap, 500×500 OHLCV, durable restart, SIGKILL
  recovery, stopped-copy + `lgraph_backup` restore, merge-base pre-series
  fixture + upgrade test.
- Clients: REST wire suite, in-process Bolt conversion suite, live TCP
  driver suite, live `neo4j==4.4.6` driver suite, bundled-client login fix
  (`fix(client)`) + regression test.
- Gate provenance (hashes, real XML parsing, freshness/completeness).
- Benchmark methodology module with baselines (never gated).
- Telemetry + financial executable examples.
- M1 identity design inputs (Option A/B) — awaiting principal decision.
- Build cost: version metadata isolated behind `lgraph::version::*`
  (`fix(build)`); a docs-only commit now recompiles 1 TU instead of ~239
  (measured). M4 fresh-client journey test with idempotent-retry guidance.
- Benchmarks: contention/latency profile (writes p50/p95/p99, scans,
  transport retries), fine-grained 1s-cadence profile (full-span vs narrow
  reads), eviction counter asserted via lifecycle metrics.
  Follow-ups landed: storage-amplification profile (caps 100/1000,
  rewrite timings), over-budget eviction-churn profile (6 graphs,
  evictions asserted), M4 client-contracts doc
  (docs/architecture/09-series-client-contracts.md: normalization rules,
  INT64/precision/timezone, null matrix, error catalog, retry guidance,
  parser matrix), SDK collection round-trip test, fresh-client journey
  test with idempotent-retry helper (smoke: individual writes, not batch;
  manual windows, not cursors), multi-row rollback regression.

Partial (landed, plan still lists remainder):

- Eviction/reopen (server-level churn green; constrained-memory observer
  and iterator/txn/store lifetime asserts open).
- Pre-series fixture (merge-base binary; true v4.5.2 files open).
- Benchmarks (methodology + small baselines; 10M/11.7M profiles need the
  M2 importer; budgets outstanding).

Open or blocked for the current GraphFin candidate:

- M1 design decision (gates Phase 1/2/5 implementation).
- Full-binary TSan (prebuilt libvsag.so → libgomp preempts interceptors).
- Full-binary ASan unit_test link (prebuilt librocksdb.a lacks RTTI).
- Genuine v4.5.2 release binary (no tags upstream).
- HA replication evidence (the latest merged series failover gate failed both
  cases and needs diagnosis).
- M5+ design reviews (bitemporal model, cursors, export format).

## Session log (archived)

- REST login fix + bundled-client regression: done (`fix(client)`).
- M1 identity design inputs (Option A/B): drafted above, awaiting principal.
- M0 follow-ups landed after the plan: eviction/reopen test
  (`--max_open_graphs 2` + idle timeout churn), live Bolt assertions via the
  real `neo4j==4.4.6` driver package, benchmark methodology
  (`test_series_bench.py`, baselines logged, never gated), telemetry +
  financial executable examples (`demo/SeriesTelemetry`, `demo/SeriesFinancial`).
- Still blocked: full-binary sanitizer runs (image), genuine v4.5.2 fixture
  (no tags upstream), HA replication evidence, M1 design decision (gates
  Phase 1/2/5 implementation).
