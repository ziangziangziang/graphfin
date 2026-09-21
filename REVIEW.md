# Development review

Review date: 2026-09-20. Scope: local `timeseries` branch at `3e9428bd`, including the uncommitted working tree, assessed against [PROJECT.md](PROJECT.md). Perspective: lead software engineer and architect. Remote branches were not fetched; deployment status and other checkouts are outside this assessment.

## Assessment

**Development follows the intended architecture and milestone order, but the feature is not ready to release.** S0 and the planned S1 foundations are committed; S2 reads are implemented locally and their existing golden suites pass. S3–S5 remain outstanding. The immediate priority should be a correctness pass across schema changes, deletion, and query results before expanding the feature surface.

There is substantial working code here: compressed buckets, transactional storage, persisted schema metadata, vertex APIs, and query reads. However, this review reproduced a serious identity defect: **deleting one series field can cause another field to return the deleted field's data.** Existing tests also miss orphaned buckets and result-type errors. Passing the current suites therefore does not establish production readiness.

I recommend retaining the core design, fixing the high-priority findings below, and then delivering one complete Cypher create/write/read/delete workflow. A first-class `FieldType::SERIES` would not resolve these defects and should remain a later decision.

## What is happening in development

The checkout contains two distinct layers of work:

- The inherited `performance` work includes graph-limit changes, lazy graph opening/LRU eviction, lifecycle metrics, backup adjustments, architecture documentation, and recorded scaling benchmarks through 100,000 graphs. These are baseline capabilities, not evidence that time-series workloads scale or recover correctly.
- Five subsequent commits implement encoding (`afffb573`), bucket storage (`c6acdb31`), schema support (`d73ac18d`), graph/transaction wiring (`0be5d791`), and nested query result transport (`3e9428bd`). The current uncommitted work implements S2 reads and summaries, parser aliases, property handling, and test fixtures.

At review start there were 11 modified tracked files, four untracked series golden files, and an untracked `PROJECT.md`. The S2 tracked diff contained 549 additions and 11 deletions. The project plan and S2 tests must be included in the eventual commits so another developer can reproduce this state.

The local merge base with `performance` is `384dc7da`. The plan's named base, `488ad42cc`, is not present in this clone. Update the provenance in the plan; this discrepancy alone does not establish a branch-management failure or an HA dependency.

| Stage | Current evidence | Assessment |
|---|---|---|
| S0: encoding | Timestamp delta-of-delta, double XOR, integer delta/RAW64, null bitmaps; eight tests pass | Implemented and committed. This review did not repeat the sanitizer runs reported in commit history. |
| S1: store + schema | One `_tseries_` table per graph; optional BLOB modifier; trailing schema extension; vertex transaction APIs; split, abort, delete-element, and reopen tests | Planned foundation implemented; existing tests pass. Schema identity and lifecycle defects prevent treating it as hardened. |
| S2: reads | All nine read functions, summary map, dotted/underscore registrations; both query golden suites pass | Implemented in the working tree, with correctness and complexity gaps below. Not yet a completed, committed milestone. |
| S3: writes + edges | Edge keys and direct edge-delete cleanup exist at storage level | Partial groundwork only. No series write/DDL procedures, REST schema metadata, or complete edge transaction/query API found. |
| S4: import | No series-specific ingestion implementation found in the import paths | Outstanding. Neither the 10M-point target nor Cypher/import equivalence is demonstrated. |
| S5: hardening | Unit-level abort and reopen coverage exists | Outstanding. `test/integration/test_timeseries.py` is absent; no series-specific server restart, backup/restore, concurrent-writer, or realistic workload evidence was found. |
| S6: first-class type | No new series enum | Appropriately deferred. The plan inconsistently calls this optional and scheduled; settle that after v1. |

The golden read fixtures populate series through internal C++ APIs. They demonstrate reads, not a customer-usable create-and-ingest workflow.

## Architectural alignment

The major choices are sound and match the plan: external bucket storage in a single graph-local table, ordinary KV transactions, no new `FieldType`, unchanged record layout, bounded bucket rewrites, and synchronous graph lifecycle ownership. Keep those choices.

Several deviations improve correctness and should be incorporated into `PROJECT.md`:

- Timestamp keys flip the sign bit before big-endian encoding, preserving numeric ordering for pre-1970 timestamps.
- The largest timestamp escape uses 64 bits, and XOR leading-zero counts use six bits. The narrower widths in the plan cannot represent all intended inputs. See [series_encoding.h](src/core/series_encoding.h) and the S0 commit explanation.
- Ordinary property writes reject series fields. This follows the explicit rejection rule in the plan and resolves its contradictory hook table, which also suggests routing ordinary SET through Upsert.
- Returning native containers required a shared result-path change. The plan's assumption that serialization was essentially free was disproved during implementation. The separate result commit is justified, but it requires compatibility testing across existing queries and clients.

The plan also promises a summary derived from only the first and last bucket headers. The specified headers contain each bucket's own count and first timestamp; they do not contain a series-wide total or the last timestamp. That promise needs either additional transactional metadata or a revised complexity contract.

## Findings and required changes

Priority **P1** means resolve before release or wider exposure. **P2** means a substantive acceptance or hardening gap. Findings marked reproduced were exercised during this review; the others are source-level findings.

### 1. P1 — Schema edits can silently return another series's data

**Reproduced.** Create a normal, non-fast-alter label with `id`, `prices`, and `quotes`; give both series one INT64 measure. Write `prices=11` and `quotes=22` at the same timestamp. Delete `prices` using `AlterLabelDelFields`. Reading `quotes` then returns **11**, not 22.

Bucket keys use the current record field ID. [schema.cpp](src/core/schema.cpp), line 1042, reassigns those IDs during ordinary layout refresh. [lightning_graph.cpp](src/core/lightning_graph.cpp), line 771, removes fields and rewrites records without migrating or deleting the corresponding series keys. This is silent data misassociation, not merely inaccessible old data.

**Recommendation:** give series fields stable persisted identities independent of record positions, or migrate affected keys atomically with schema edits. Until that is implemented, explicitly reject schema operations that invalidate existing series identities. Cover deletion of preceding ordinary fields, deletion/recreation of series fields, additions that reorder fields, and measure redefinition in both schema modes.

### 2. P1 — Bulk deletion bypasses series cleanup

**Reproduced:** write one series bucket, call `LightningGraph::DropAllVertex()`, and count `_tseries_` keys. One bucket remains; zero were expected.

[lightning_graph.cpp](src/core/lightning_graph.cpp), lines 67–123, clears graph records, indexes, and detached properties but omits the series table. Label deletion also calls lower-level graph deletion directly. Additionally, the incident-edge callback in [transaction.cpp](src/core/transaction.cpp), line 447, has no series cleanup; the direct `DeleteEdge` path does. The incident-edge omission becomes critical when S3 enables edge writes.

**Recommendation:** enumerate every destructive graph/schema operation and make series cleanup part of its transaction. Clear the table when clearing all vertices; clean the appropriate prefixes for label/field deletion and both directions of incident-edge deletion. Add rollback checks and verify physical key removal, not only empty query results.

### 3. P1 — Nested result conversion changes string values into containers

**Reproduced on both parser paths:**

```cypher
RETURN ['[]', '{}'] AS xs;
```

Actual result: `[{"xs":[[],{}]}]`. Expected result: `[{"xs":["[]","{}"]}]`.

The new recursive converter in [op_produce_results.h](src/cypher/execution_plan/ops/op_produce_results.h), line 37, delegates scalar conversion to `FieldDataToJson`. That existing helper parses strings containing JSON arrays or objects ([json_convert.h](src/server/json_convert.h), line 70). Reusing it for typed nested values preserves that coercion where literal strings must remain strings.

**Recommendation:** use a type-preserving scalar conversion inside the recursive result path. Test strings resembling JSON, mixed nesting, nulls, empty containers, large integers, and temporal values through REST, Bolt, and Python. The existing golden suites passing is useful but does not cover these transport contracts.

### 4. P1 — Integer aggregation can overflow, including during min/max

**Source-level finding.** [arithmetic_expression.cpp](src/cypher/arithmetic/arithmetic_expression.cpp), line 1597, unconditionally evaluates `isum += v.i` for integer measures. Two valid INT64 inputs can overflow the signed accumulator. This occurs even for `series.min` and `series.max`, which do not need a sum. Integer mean also sums before conversion to double.

**Recommendation:** accumulate only what each aggregate requires. Specify checked overflow behavior for integer sums and use a suitable wider or floating accumulator for means. Add boundary tests and UBSan coverage. Do not silently wrap or lose precision without an explicit API contract.

### 5. P1 — A failed bucket split can leave a committable partial update

**Source-level finding.** [series_store.cpp](src/core/series_store.cpp), lines 491–501, explicitly acknowledges that the left split may have been stored before the right split fails. `SetVertexSeriesPoint` simply returns that boolean ([transaction.cpp](src/core/transaction.cpp), line 1629); it does not abort or invalidate the transaction.

A caller that handles `false` and later commits can therefore persist part of an unsuccessful operation. Sharing a KV transaction guarantees rollback when the transaction aborts; it does not automatically make a failed method leave the transaction unchanged.

**Recommendation:** encode and validate all replacement buckets before mutating storage, or enforce transaction invalidation on this failure. Add a constrained-byte-cap correction test that fails after a possible left split, then attempts to commit and verifies the original data remains intact or commit is refused.

### 6. P2 — Point lookups and latest/earliest reads perform a full-history scan

**Source-level finding.** Every read calls `ResolveSeriesTarget`, which calls `ProbeVertexSeries` to obtain names and validate the field. The probe performs an unbounded count plus earliest/latest reads ([transaction.cpp](src/core/transaction.cpp), lines 1603–1611). `Count` decodes every bucket's timestamp column before deciding whether its whole count can be used ([series_store.cpp](src/core/series_store.cpp), lines 618–630).

Consequently, `series.at`, `series.latest`, and even a narrow range pay work proportional to the entire series before executing their intended lookup. `RETURN n.prices` has the same full-history timestamp cost. Aggregates additionally materialize every selected point and every measure in memory.

**Recommendation:** separate schema resolution from summary computation. A point lookup should validate schema without reading buckets. Introduce header-only counting and, if constant-cost summaries are required, maintain count/first/last metadata in the same transaction. Stream aggregates over buckets with measure selection. Add operation-count or scaling tests that prove a narrow lookup does not scan unrelated history.

### 7. P2 — Returned maps do not fully support normal nested access

**Reproduced on Cypher:**

```cypher
MATCH (p:Person {name:'Rachel Kempson'})
WITH p.prices AS s
RETURN s.measures AS names;
```

The result is `[CypherException] Not found or type mismatch`. The map-access branch in [record.cpp](src/cypher/resultset/record.cpp), lines 85–95, still accepts only scalar entries, although `measures` is a list.

**Recommendation:** return the complete `cypher::FieldData` from map member access. Test summaries and points through projection, WITH, list access, and UNWIND where each parser supports them. Also reserve the measure name `ts`: schema validation currently accepts it, while `PointToMap` inserts the timestamp under that key first and silently fails to insert the measure of the same name.

### 8. P2 — Verification infrastructure does not enforce milestone success

Both [ci.yml](.github/workflows/ci.yml) and [phase0-baseline.yml](.github/workflows/phase0-baseline.yml) restrict push/PR events to `main`; this clone tracks `master`, `performance`, and `timeseries`, with `origin/HEAD` pointing to `origin/master`. Manual/scheduled execution is still possible, but these filters do not automatically gate the current branch flow.

More importantly, [run_tests_inner.sh](ci/phase0/run_tests_inner.sh) records test exit codes and then exits zero. That is an intentional Phase 0 baseline policy, unsuitable as a feature acceptance gate. `TestQuery.TestDemo` also runs without golden comparison; its Cypher path can report a passing test while placing an exception in the output, as the summary-access probe demonstrated.

**Recommendation:** add a dedicated series CI job on the actual development/integration branches. Require nonzero failure status for series tests, crashes, missing results, and new regressions. Keep historical upstream exceptions explicit and narrowly scoped. Record commit ID, working-tree state, image digest, build flags, and test outputs together.

## Verification performed

The existing Linux ARM64 `build/output/unit_test` was run in `tugraph-compile-arm64:phase0`. The repository was mounted read-only; writable query fixtures and databases were copied into container-local temporary directories.

| Check | Result |
|---|---|
| `TestSeries*:*Schema*Series*` | **35/35 passed:** 8 encoding, 13 store, 4 transaction, 10 schema cases |
| `TestQuery.TestCypherSuite:TestQuery.TestGqlSuite` | **2/2 passed**, including the new series files and expected rejection of dotted names in GQL |
| Cypher field-data test plus V2 function, expression, aggregate, crash-regression, and edge-ID suites | **6/6 passed** |
| Temporary C++ regression probes, compiled against current headers and linked with existing project build objects | **2/2 failed their expected-correctness assertions:** orphaned bucket and cross-field data misassociation, described above |
| Additional query-output probes | Confirmed nested string coercion and Cypher summary-list access error |
| `git diff --check` | Passed |

The **43 passing existing tests** are targeted evidence, not a full repository test result. The temporary failing probes were review diagnostics, not committed test changes. Their setup and observed values are specified in findings 1 and 2.

This review did **not** perform a clean rebuild, full unit/integration suite, new sanitizer run, client transport integration, or series-specific backup/restart/concurrency benchmark. The local `build-s2-final.log` reports a successful unit-test build, but a historical log is not a clean-build certification of this exact checkout. The cached configuration has `ENABLE_ASAN=OFF`. The older [test baseline](docs/architecture/09-test-baseline.md) records an upstream failure and intermittent crashes; those historical results must not be presented as current series validation.

## Recommended delivery sequence

1. **Stabilize identity and transactional behavior.** Resolve findings 1, 2, and 5; make their reproductions permanent tests. Decide whether schema mutation migrates series data or rejects unsupported operations. Do this before exposing series DDL to users.
2. **Finish S2 as a reviewable commit.** Fix result fidelity, overflow, nested access, and metadata-only resolution. Isolate the series fixture instead of unconditionally augmenting every YAGO fixture in `GraphFactory::create_yago`; hiding series fields from `keys()`/property enumeration should be an explicit product decision, not a way to preserve unrelated golden outputs.
3. **Deliver S3 end to end.** Implement vertex/edge DDL and metadata discovery, append/set/clear, edge reads, and cleanup. Acceptance should create the schema and all points through Cypher, correct one measure without losing the others, read through supported clients, repeat writes deterministically, and delete cleanly. Preserve authorization checks and procedure read/write classification.
4. **Build S4 around bucket batches.** Avoid implementing 10M-point import as repeated one-point upserts. Specify duplicate ordering, timestamp/null/type validation, and transactional batch boundaries. The plan's byte-for-byte equality requirement needs canonical bucket partitioning: current splits depend on insertion order, and descending backfill creates one-point buckets. Either guarantee canonical output or explicitly choose logical equivalence and change the acceptance criterion.
5. **Complete S5 with evidence.** Run server restart and durable recovery, backup/restore, graph eviction/reopen, eight writers on distinct series, same-series contention/retries, and the stated 500-symbol OHLCV workload. Record compression, throughput, latency, memory, abort rate, and reproducible configuration. Loading a schema produced by the new writer is insufficient proof of v4.5.2 compatibility; test an actual old database fixture.

Update `PROJECT.md` with the actual branch base, implemented encoding format, schema identity rules, result semantics, summary complexity, import equivalence definition, and milestone evidence. Retain the stated exclusions for TTL, rollups, string measures, cross-series aggregates, and sharding interaction. The useful next delivery is a correct, testable S2/S3 workflow; the outstanding work is identifiable without redesigning the storage engine.
