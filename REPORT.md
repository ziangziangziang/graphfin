# Response to REVIEW.md (2026-09-21) — R1–R6 fixes

Branch: `timeseries`. Review HEAD was `de8357c5`. Fixes committed as
`f869df41` (fix) + `0adb46e2` (test); this report and the PROJECT.md cleanup
as the docs commit on top.
Gate: 93/93 pass on the review's acceptance selection plus 7 new regression
tests (86 pre-existing incl. the review's 2 fixture-dependent passes, 5 new in
`test_series_transaction.cpp`, 1 new ×2 params in `test_schema.cpp`, plus the
extended `series_write` Cypher golden). Incremental container build
(`tugraph-compile-arm64:phase0`, `RelWithDebInfo`, `-Wall -Werror`) is clean.
The gate ran before the final comment/docs-only edits (`record.cpp` summary
comment, PROJECT.md passages); those change no behavior.

## What changed, per finding

### R3 — decode counter race (fixed)
`SeriesStore::decode_count_` was a shared `mutable size_t` incremented on every
`Range`/`Count`/`Latest`/`Earliest`/existing-bucket `Upsert`.
Now `mutable std::atomic<size_t>` with relaxed load/store/fetch_add
(`src/core/series_store.h`, `src/core/series_store.cpp`), and the header no
longer claims the counter is off production paths. New
`ConcurrentReadersShareTheStoreSafely` (8 readers × 25 iterations over one
graph) exercises the path; a ThreadSanitizer run with concurrent readers is
still outstanding (no TSan build was done here).

### R4 — LIST/MAP measure values stored as null (fixed)
`ParseSeriesMeasureValue` (`src/cypher/procedure/procedure.cpp`) mapped every
non-scalar to `Null()`. Now only an explicit null scalar yields `Null()`;
LIST/MAP throws before any mutation, so the stored point is preserved.
Golden coverage in `series_write.test/.result`: `series.update` with `[99]`
and `series.append` with `{v:99}` both error and the point still reads
`close:12.5, volume:7`.

### R5 — bucket options wrapped before validation (fixed)
`CreateSeriesFieldImpl` cast signed input to `u32`/`u64` before checking zero.
Now it rejects unknown option keys, then range-checks the signed values:
`bucket_max_points` in `[1, 1000000]`, `bucket_max_bytes` in `[1, 16777216]`
(16 MiB property cap), `bucket_max_span_us >= 0`. The point/byte caps are
mirrored in `CheckSeriesFieldSpec` (`src/core/schema.cpp`) so direct-API DDL
cannot bypass them. Golden coverage: `2^32+1`/`-1`/`-1` combo, unknown key,
and a follow-up valid create proving rejected DDL leaves no field behind.

### R6 — timestamp write/read domain mismatch (fixed)
`ParseSeriesTimestamp` accepted any INT64 while reads always build `DateTime`.
Added `series::IsValidSeriesTimestamp` (`src/core/series_types.h`) for the
DATETIME domain; enforced in `ParseSeriesTimestamp` (Cypher), in
`Transaction::SetVertex/EdgeSeriesPoint` (`InputError`), and defensively in
`SeriesStore::Upsert` (returns false). `kMinTs`/`kMaxTs` remain valid as
unbounded range-query sentinels. Also fixed the signed-subtraction overflow in
the span-cap check (`series_store.cpp`) to unsigned subtraction. Golden
coverage: `ts:9223372036854775807` rejected; C++ test pins both INT64 extremes
rejected and both DATETIME extremes accepted.

### R2 — fast-alter series DDL wrote an un-reopenable schema (fixed)
`AlterLabelAddFields` called `SetDefaultValue` for every added field, stamping
`set_default_value=true` even for null, which reopen validation rejects.
The fast-alter path now skips `SetDefaultValue` for series fields
(`src/core/lightning_graph.cpp`); construction-time validation stays strict
(`must not have a default value`). Recovery for databases already written with
the invalid combination: `Deserialize` in `src/core/schema.h` strips a *null*
default flag from series fields on load (a null default carries no value and
the record keeps no bytes for a series field); non-null defaults stay
rejected. New tests: `FastAlterAddSeriesFieldSurvivesReopen` (add series to an
existing fast-alter label, write, reopen, read back) and
`NullDefaultSeriesFlagIsStrippedOnLoad` (round-trips the pre-fix persisted
form). The pre-existing `RejectsInvalidSeriesFields` still passes unmodified.

### R1 — series identity vs reload reordering (triggering DDL rejected)
Confirmed the mechanism: `SetSchema` groups fixed-width before variable-width
fields while `AddFields` appends live, so adding a fixed-width field behind a
series field (BLOB = variable-width) shifts series ids on reopen and orphans
buckets (within-type relative order is preserved, so ordinary record data is
unaffected — only positional bucket keys break). Per the review's sanctioned
interim ("if a complete fix must wait, reject the triggering schema alteration
before it commits"), packed-layout DDL that would leave any series field's
live id different from its reload-canonical id is now rejected in
`AlterLabelAddFields`, `AlterLabelModFields` (type changes that flip
fixed/variable classification included), and `AlterLabelDelFields`
(`src/core/lightning_graph.cpp`, helper `SeriesReloadIdWouldShift`).
Fast-alter labels (stable ids) are exempt. Also closed the
ordinary-field-to-series conversion path in `ModFields`: gaining the modifier
would silently hide ordinary stored data, so it is rejected. New tests:
`AddingFixedFieldThatWouldShiftSeriesIdsIsRejected` (fixed add rejected,
variable add allowed, data verified live and after reopen) and
`OrdinaryFieldCannotGainTheSeriesModifier`. The persisted-identity redesign
(name-keyed buckets or stable series ids with migration) remains a
principal-engineer decision; the guards make the current format safe until then.

## Validation
- `TestSeriesTransaction.*`: 18/18 (13 existing + 5 new).
- `TestQuery.TestCypherSuite` + `TestGqlSuite`: pass, incl. extended
  `series_write` golden (R4/R5/R6 + no-residue follow-up create).
- Full gate selection (series/schema/transaction/lgraph/detach/cypher/gql
  suites): **93/93**.
- One iteration finding: the first R2 approach (tolerate null defaults in
  validation) broke `RejectsInvalidSeriesFields`; reworked to load-time
  stripping, strict validation intact.

## Outstanding (for principal review)
1. Sanitizer runs: TSan (R3 readers + reader/writer overlap), ASan/UBSan with
   decoder fuzz (`DecodeBucket` trusts header counts) — not run here.
2. S5 matrix: durable restart/recovery, backup/restore, eviction/reopen,
   distinct-series + same-series writer contention, OHLCV workload, real
   old-version fixture.
3. R1 follow-up: persisted series identity independent of record layout (the
   current reject-guards are the review's sanctioned interim, and they make
   adding fixed-width fields to packed-layout series labels an error — a real
   limitation to lift with the redesign).
4. Docs/gate: `PROJECT.md` cleaned in this pass (single merge-base statement,
   accurate S1–S3 acceptance status, corrected Decision 2 heading and timestamp
   codec description, series-summary cost comment fixed at the source).
   Remaining: gate provenance (source/binary hashes) and per-run XML handling
   per review §Harden.
 5. `git status` is otherwise clean: no implementation files changed outside the
    list above; no scratch artifacts committed (golden `.real` files are
    gitignored and were removed). `REVIEW.md`/`AGENTS.md` workspace entries
    predate this work and are left uncommitted.

---

# S5 + sanitizer follow-up (principal's 4 trackers)

Branch state at write time: R1–R6 commits `f869df41`, `0adb46e2`, `c7c72e95`
below; this chapter's work uncommitted, awaiting review. Gate after all work:
**102/102 ACCEPT** (fresh timestamped XML, real-parser provenance);
integration pytest **6/6**; TSan harness clean; ASan/UBSan fuzz (20k iters)
clean. The principal has since replaced `PROJECT.md` with the M0–M8 forward
plan; that rewrite is not mine and stays uncommitted here.

## 1. Sanitizers

- **Options**: `ENABLE_TSAN` / `ENABLE_UBSAN` added to `Options.cmake`
  (alongside existing `ENABLE_ASAN`); TSan builds also drop `-fopenmp`
  (keeping `-pthread` at link and `-Wno-unknown-pragmas`) because libgomp
  preempts TSan's pthread interceptors at init.
- **TSan**: the full `unit_test` binary cannot run under TSan in the pinned
  image — the prebuilt `libvsag.so` (OpenMP) is linked into every process and
  breaks gcc-8.4 TSan init (`failed to intercept pthread_mutex_trylock`;
  confirmed via ldd dependency tracing). Coverage instead via a minimal harness
  (`ci/phase0/sanitizer/tsan_series_race.cc`) linking only the
  TSan-instrumented store objects: 4 readers × 200 Range/Count/Latest +
  writer × 50 Upserts over one shared `SeriesStore` → **clean, no races**.
  Run requires `--security-opt seccomp=unconfined` (Docker blocks the ASLR
  personality syscall) and a 1 GiB LMDB map (default 4 TB + TSan shadow
  exceeds the container VA budget).
- **TSan found (fixed)**: `ThreadIdAssigner::ReleaseThreadId`
  (`src/core/thread_id.h`) wrote the occupancy array unlocked while
  `GetThreadId` runs under mutex — a pre-existing upstream race on a path
  every transaction touches. Now takes the same lock.
- **ASan+UBSan** (`ENABLE_ASAN=ON ENABLE_UBSAN=ON`, one build): full
  `unit_test` link is blocked in this image (prebuilt `/usr/lib64/librocksdb`
  provides no `typeinfo for rocksdb::*`; the server link fails identically
  with or without series changes — pre-existing bit-rot, upstream ASan flow
  uses `WITH_TEST=OFF`). Coverage instead via a minimal harness
  (`ci/phase0/sanitizer/asan_series_fuzz.cc`): 20k deterministic adversarial
  decoder mutations (bit flips, overwrites, truncations, header extremes
  incl. count=UINT32_MAX, splices) plus store round-trips → **clean**.
- **UBSan found (fixed)**: misaligned `size_t` loads of the LMDB value
  prefix in `LMDBKvTable::GetVersion`/`GetValue(for_update)`
  (`src/core/lmdb_table.cpp`) — pre-existing UB on every write-txn read,
  fatal on strict-alignment ARM. Now `memcpy`.
- **Decoder hardening** (review §Harden): `DecodeBucket`/
  `DecodeBucketTimestamps` capped allocations at 2^20 points before trusting
  the header count (schema caps buckets at 10^6); hostile counts return false
  with no multi-GB allocation. Fuzz battery committed as
  `TestSeriesStore.AdversarialBucketMutationsNeverCrash` (gate).
- Not run: full-binary TSan (vsag, above), full-binary ASan unit_test
  (rocksdb link, above). Both need image/toolchain fixes outside this stream.

## 2. S5 lifecycle matrix (all in gate or pytest)

- 8 distinct-series writers, same-series contention with `TxnConflict`
  retry to exact coverage **plus logged abort rate**, reader/writer overlap,
  500-symbol × 500-day OHLCV (250k points, exact counts, single-bucket
  packing, bytes/point vs 32 raw logged) — `test_series_transaction.cpp`.
- Durable restart (committed points + schema survive full stop/start, RPC +
  REST verified), SIGKILL under `--durable` (all acknowledged commits fully
  present), stopped-copy backup/restore, real `lgraph_backup` tool
  backup/restore — `test/integration/test_timeseries.py` (6/6).
- Pre-series upgrade path — `DatabaseWithoutSeriesGainsItOnUpgrade`; plus a
  **genuine pre-series fixture**: `test/resource/data/preseries_db/data.mdb`
  written by `lgraph_server` built at merge base `384dc7da`, opened and
  upgraded by `PreSeriesReleaseFixtureUpgradesCleanly` (2 vertices + edge
  verified, series DDL added, points survive reopen).
- Remaining per the new M0 plan: eviction under constrained memory, actual
  v4.5.2-binary fixture (ours is the merge-base binary, one step removed),
  HA replication evidence.

## 3. REST/Bolt clients (principal item 3)

- **Found by testing**: reading any MAP/LIST cell (every series read) over
  REST/RPC **aborted the server** (`FMA_ASSERT(false)` in
  `FieldDataConvert::FromLGraphT`, `src/server/proto_convert.h`): the plan
  header for function calls is `ANY`, but the converter dereferenced
  `v.fieldData` for collection elements that only set `v.map`/`v.list`.
  Fixed by dispatching on the element's type; collections keep the
  long-standing JSON-text crossing. Pre-existing bug, exposed by series.
- **REST** (`test_timeseries.py` over real HTTP): structured assertions for
  points/summaries/ranges (parsed from JSON text), INT64 extremes, nulls,
  empties, JSON-looking strings, error preservation. Note: the bundled
  `TuGraphRestClient` login is stale (posts `userName`, server requires
  `user`); the test drives HTTPX directly with `Bearer` tokens.
- **Bolt**: new `test_series_bolt.cpp` pins `Result::BoltRecords()` shapes
  (scalars native incl. `LocalDateTime{sec,nano}`, collections as JSON text),
  plus a **live packstream driver** (`test/integration/bolt_driver.py`,
  no driver package in env) asserting the same over TCP, incl. session
  RESET-after-FAILURE behavior.
- Wire contract documented in `PROJECT.md` Decision 2 (since rewritten by
  the principal into the M0–M8 plan, which states it independently).

## 4. Old-version fixture + gate provenance (principal item 4)

- Fixture: see §2 (merge-base binary + committed `data.mdb` + upgrade test).
- Gate (`ci/phase0/run_series_tests.sh`): fresh timestamped XML per run
  (+ `series-latest.xml` copy, stale `series.xml` removed), real
  `xml.etree` parsing with freshness (`mtime >= run start`) and completeness
  (`testcase` count == `tests`) checks, dirty-build provenance (`git diff`
  sha256, per-file sha256 of untracked content), binary hashes
  (`unit_test`, `liblgraph.so*`), actual `CMakeCache` options, recorded
  `SKIP_BUILD`/`CLEAN` flags. Latest run: ACCEPT 102/102, 0 excused.
