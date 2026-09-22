# Progress report to the principal engineer

Date: 2026-09-22. Branch: `timeseries`. HEAD at write time: see commit list
in §7. Pinned build: `tugraph-compile-arm64:phase0`, `RelWithDebInfo`,
`-Wall -Werror`, container `linux/arm64`.

## 1. Status summary

| Area | Status | Evidence |
|---|---|---|
| R1–R6 review findings | Fixed, approved for merge/sign-off | Prior review §Post-fix; regressions in gate |
| Decoder hardening + fuzz | Done | Cap + `AdversarialBucketMutationsNeverCrash` (gate); 20k-iter ASan/UBSan harness clean |
| Sanitizer integration | Partial | TSan + ASan/UBSan harness runs clean; full-binary runs blocked by image issues (details §3) |
| Upstream defects found by testing | Fixed (3) | Unlocked thread-id release; misaligned LMDB loads; ANY-cell server abort |
| S5 lifecycle matrix | Done | Contention/OHLCV (C++), restart/SIGKILL/backup incl. real tool (pytest) |
| Eviction/reopen | Done | Churn test + lifecycle `evictions` counter asserted |
| Pre-series fixture | Partial | Merge-base-binary fixture + upgrade test; true v4.5.2 files open |
| REST/Bolt clients | Done | REST suite, in-process Bolt suite, live TCP-driver suite, live `neo4j` driver suite, bundled-client login fix |
| Bundled REST login mismatch | Fixed | `fix(client)` + regression test |
| Multi-row rollback | Proven + tested | Live verification, REST regression |
| Wire contract | Documented | `docs/architecture/09-series-client-contracts.md`, verified per transport |
| Benchmark methodology | Done (baselines, never gated) | OHLCV, contention, fine-grained, storage, overbudget profiles |
| Gate provenance | Done | Hashes, real XML parsing, freshness/completeness |
| Build cost (commit → ~239 TUs) | Fixed, measured | Version-metadata isolation; docs-only commit now rebuilds 1 TU |
| Executable examples | Done | Telemetry + financial (stdlib-only, verified live) |
| M1 persisted identity | Design inputs drafted | PLAN.md; decision needed before implementation |
| HA replication evidence | Not started | Needs multi-node setup |

Current results: **gate 103/103 ACCEPT** (fresh XML, provenance), **pytest
15/15** (10 series + 5 bench), TSan harness clean, ASan/UBSan fuzz clean.

## 2. R1–R6 (approved; retained for the record)

- **R1**: packed-layout DDL that would shift series ids on reopen is rejected
  (`AlterLabelAddFields/ModFields/DelFields`, `SeriesReloadIdWouldShift`);
  ordinary→series conversion rejected; fast-alter exempt. Persisted identity
  redesign remains a principal decision (see §6).
- **R2**: fast-alter add skips defaults for series fields; pre-fix null-default
  flags stripped on schema load; construction validation stays strict.
- **R3**: shared decode counter is `atomic`; TSan harness over concurrent
  readers + writer overlap is clean (§3).
- **R4**: LIST/MAP measure values throw before mutation; stored point
  preserved; multi-row rollback proven live and pinned in REST tests.
- **R5**: option keys + signed ranges validated before narrowing
  (`[1,10^6]` points, `[1,16MiB]` bytes, `span >= 0`), mirrored in schema
  validation; rejected DDL leaves no field.
- **R6**: stored timestamps must fit the DATETIME domain (procedure, both
  transaction entry points, store); `kMinTs/kMaxTs` stay range sentinels;
  span-cap arithmetic overflow fixed.

## 3. Sanitizers: runs, finds, and blockers

- `ENABLE_TSAN` / `ENABLE_UBSAN` added (`Options.cmake`); TSan builds drop
  `-fopenmp` (link-only `-pthread`, `-Wno-unknown-pragmas`).
- **Full-binary TSan is blocked in this image**: prebuilt `libvsag.so`
  (OpenMP) is linked into every process and breaks gcc-8.4 TSan init
  (`failed to intercept pthread_mutex_trylock`, traced via ldd). Coverage
  instead: minimal harness (`ci/phase0/sanitizer/tsan_series_race.cc`)
  linking only TSan-instrumented store objects — 4 readers × 200
  Range/Count/Latest + writer × 50 Upserts over one shared `SeriesStore` →
  **clean**. Needs `--security-opt seccomp=unconfined` (ASLR personality)
  and a 1 GiB LMDB map (4 TB default + TSan shadow exceeds VA budget).
- **Full-binary ASan `unit_test` link is blocked**: prebuilt
  `/usr/lib64/librocksdb` provides no `typeinfo for rocksdb::*`; the server
  link fails identically with or without series changes. Coverage instead:
  minimal harness (`ci/phase0/sanitizer/asan_series_fuzz.cc`), 20k
  deterministic adversarial decoder mutations → **clean, no crashes/hangs**.
- **Found and fixed**: unlocked `ThreadIdAssigner::ReleaseThreadId`
  (upstream race, every transaction touches it); misaligned `size_t` loads
  in `LMDBKvTable::GetVersion`/`GetValue(for_update)` (UB, fatal on strict
  ARM); decoder allocation cap at 2^20 points before trusting the header
  count (schema caps buckets at 10^6).

## 4. Lifecycle, eviction, fixture

- C++ gate: 8 distinct writers, same-series contention with `TxnConflict`
  retry to exact coverage plus logged abort rate, reader/writer overlap,
  500×500 OHLCV (counts, packing, bytes/point), read-snapshot isolation
  across concurrent commit, pre-series upgrade path.
- pytest: durable restart (RPC+REST), SIGKILL under `--durable` (all
  acknowledged commits present), stopped-copy restore, real `lgraph_backup`
  tool restore, eviction/reopen with the lifecycle `evictions` counter
  asserted (idle task runs ≥5s periods and skips referenced graphs —
  encoded in the test).
- Fixture: `test/resource/data/preseries_db/data.mdb` written by
  `lgraph_server` built at merge base `384dc7da`; opened, verified,
  upgraded with series DDL, and re-verified after reopen.

## 5. Clients

- **Server abort found by testing, fixed**: any MAP/LIST cell over REST/RPC
  aborted the server (`FMA_ASSERT(false)` in `FromLGraphT`: plan header
  `ANY` vs collection element union). Fix dispatches on the element type;
  collections keep the JSON-text crossing. Pre-existing, exposed by series.
- REST suite (real HTTP): points/summaries/ranges, INT64 extremes, nulls,
  empties, JSON-looking strings, error preservation, multi-row rollback.
- Bolt: in-process `BoltRecords()` suite + live packstream driver suite +
  live `neo4j==4.4.6` driver suite (scalars native incl. `LocalDateTime`,
  collections as JSON text, RESET-after-FAILURE).
- Bundled `TuGraphRestClient` repaired (login field, envelopes, Bearer,
  error keys) with regression test. Transport truth documented:
  REST/Bolt serve collections as JSON text for clients to parse; RPC JSON
  re-parses into objects.
- `docs/architecture/09-series-client-contracts.md`: rules, null matrix,
  error catalog, retry guidance (`idempotent_retry`: transport failures
  retried, semantic errors never), parser-support matrix, examples notes.

## 6. Build cost: version metadata isolated (measured)

New commits regenerated `src/core/version.h` (git hash) on every build, and
`core/defs.h` pulled it into ~239 TUs. Now `lgraph::version::*` accessors
(`src/core/version_info.h`) expose the same values; `version_info.cpp` is
the only TU including the generated header. Call sites migrated (banner,
welcome, `system.info`, REST info, galaxy stamping, plugin hashes).
Measured: a docs-only commit now recompiles **1 TU** (+25 relinks), down
from ~239 compilations. Follow-on workflow: `CLEAN=0` incremental builds;
`SKIP_BUILD=1` gate reruns; `GTEST_FILTER` for test selection (not compile
scope). Remaining build-tech debt (test-executable split, per-target
selection) intentionally deferred.

## 7. Benchmarks (methodology, never gated)

`test_series_bench.py`: OHLCV ingest/throughput/latency/RSS/bytes-point,
contention (writes p50/p95/p99, scans, retries), fine-grained 1s cadence
(full-span vs narrow reads), storage across caps, over-budget eviction
churn. Baselines logged with seed/config; budgets outstanding per plan.
`demo/SeriesTelemetry` + `demo/SeriesFinancial`: stdlib-only
declare→ingest→query→export, verified live.

## 8. Open items and decisions needed

1. **M1 identity decision (Option A/B in PLAN.md)** — gates all Phase
   1/2/5 implementation. Guards retained meanwhile per plan.
2. **Image fixes** (outside this stream): OpenMP-free vsag (or newer GCC)
   for full-binary TSan; RTTI-complete rocksdb for full-binary ASan.
3. **True v4.5.2 fixture** (no tags upstream to build from) and downgrade
   policy text.
4. **HA replication evidence** (multi-node setup).
5. **M5+ designs** (bitemporal model, cursors, export format) per plan rule.
6. **Benchmark budgets** and 10M/11.7M profiles (need M2 importer).

## 9. Commit list (this report covers through HEAD)

R1–R6: `f869df41` fix, `0adb46e2` test, `c7c72e95` docs. S5/sanitizer:
decoder cap, `fix(core)` races/UB, `fix(server)` ANY crash, S5/REST/Bolt/
fixture tests, sanitizer options/harnesses/gate provenance, evidence
report. Phases: REST login `fix(client)` + regression, `PLAN.md` build
plan, live-driver/eviction/bench tests, demos, client-contracts doc,
`fix(build)` version isolation + lifetime test. `PROJECT.md`/`REVIEW.md`
rewrites are the principal's and stay uncommitted, as does `AGENTS.md`.
