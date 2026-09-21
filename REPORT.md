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
