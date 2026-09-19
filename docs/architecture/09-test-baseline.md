# 09 — Test Baseline (Phase 0)

Recorded from the pinned arm64 environment (`ci/phase0/images.lock`), commit
`672e4b199`, build flags per `ci/phase0/images.lock` (`RelWithDebInfo`,
`BUILD_PROCEDURE=OFF`, `WITH_TESTS=ON`).

Reproduce with:

```bash
ci/phase0/doctor.sh
JOBS=2 ci/phase0/build.sh
ci/phase0/run_tests.sh ut    # upstream suites
ci/phase0/run_tests.sh it    # integration suites
```

Machine-readable results and full logs: `phase0-results/summary.json`,
`phase0-results/ut.log`, `phase0-results/unit_test.xml`, `phase0-results/it.log`.

## 1. Build

Clean build succeeds with **zero errors and zero warnings-as-errors**. All
binaries present: `lgraph_server`, `lgraph_cli`, `lgraph_import`,
`lgraph_export`, `lgraph_backup`, `lgraph_binlog`, `lgraph_monitor`,
`lgraph_peer`, `lgraph_warmup`, `lgraph_validate`, `lgraph_peek`, `mdb_stat`,
`liblgraph.so`, `liblgraph_python_api.so`, `liblgraph_client_python.so`,
`unit_test`, `fma_unit_test`.

Two upstream arm64 blockers had to be resolved first; see
[07](07-scalability-risks.md) R14:

| Blocker | Resolution |
|---|---|
| `-msse4.2` applied unconditionally (x86-only) | `build.sh` auto-detects aarch64 and passes `-DENABLE_BUILD_ON_AARCH64=ON` |
| `lgraph_db_python` needs Cython 3.0; image has 0.29.37 | derived image `ci/phase0/env/Dockerfile.phase0` adds Cython 3.0.0 |

Plus one resource blocker: `-j4` OOM-kills `cc1plus` on a ~7.8 GiB VM
(`cypher/execution_plan/*.cpp` TUs need 1.5–2.5 GiB each). The default is now
`-j2`, which is both safe and measurably faster than `-j4` swapping.

## 2. Upstream unit tests

### `fma_unit_test` — **PASSED**

Exit code 0.

### `unit_test` (gtest) — **1 failure out of 291**

| Metric | Value |
|---|---|
| Tests run | 291 |
| Passed | 277 |
| Failed | **1** |
| Disabled | 0 |
| Errors | 0 |
| Skipped (`GTEST_SKIP`) | 13 |
| Wall time | 932.8 s (15.5 min) |
| Exit code | 1 |

Source: `phase0-results/unit_test.xml`.

#### The failing test

```
[  FAILED  ] TestCypherV2.TestProcedure (29442 ms)
test/test_cypher_v2.cpp:332: Failure
Value of: false
  Actual: false
Expected: true
```

`test/test_cypher_v2.cpp:332` is the golden-file comparison at the end of
`TestCypherV2::TestProcedure`:

```cpp
if (diff_file(real_file, result_file)) {
    fma_common::LocalFileSystem fs;
    fs.Remove(db_dir_);
} else {
    UT_EXPECT_TRUE(false);      // line 332
}
```

The test runs a scripted query file, writes the produced results to
`real_file`, and `diff_file` compares them against the expected `result_file`.
`diff_file` returns true when they match, so the failure means **the produced
results differ from the expected results**.

The last values printed before the failure are a PageRank-style convergence
series:

```
delta(0)=1.000000
delta(1)=0.328628
...
delta(9)=0.000064
```

**Status: recorded, not diagnosed, not fixed** — per the Phase 0 policy agreed
for this phase. Two candidate explanations, neither confirmed:

1. an architecture-dependent floating-point difference (the expected file was
   most likely generated on x86 `centos7`), which would make this
   environment-specific rather than a product defect; or
2. a genuine behavioural difference in the v2 procedure path.

Note that a real v2 correctness defect *was* found independently (F1 in
[08](08-correctness-findings.md)), so (2) cannot be dismissed. This test should
be triaged early in the next phase, starting by diffing `real_file` against
`result_file` to see whether the mismatch is numeric noise or structural.

## 3. Phase 0 integration and failure tests

All 33 pass against a freshly built server.

| Suite | Tests | Focus |
|---|---|---|
| `test_multi_graph_lifecycle.py` | 11 | graph create/delete/list/reopen, cross-restart persistence, registry consistency |
| `test_multi_graph_crud.py` | 7 | CRUD, schema-per-graph, unique index enforcement, statement atomicity, cross-graph isolation, persistence across restart |
| `test_multi_graph_acl.py` | 8 | per-graph FULL/READ/NONE enforcement, denied writes do not mutate, ACL survives restart |
| `test_restart_and_failure_recovery.py` | 4 | graceful restart, SIGKILL while idle, SIGKILL mid-transaction (atomicity), repeated kill/restart cycles |
| `test_backup_restore_scale.py` | 3 | snapshot tree completeness, restore into a fresh dir, source still usable after snapshot |
| **Total** | **33** | |

Run `ci/phase0/run_tests.sh it`; wall time ~19 s.

### Notable expectations encoded in these tests

- **SIGKILL mid-transaction must be atomic.** The test issues one large
  comma-separated `CREATE` (a single transaction), kills the server mid-flight
  with SIGKILL, restarts, and asserts the graph contains either 0 or all
  vertices — never a partial set.
- **Durability is not asserted.** LMDB runs with `MDB_NOSYNC`
  (`src/core/lmdb_store.cpp:60-65`) and `durable` defaults to false
  (`src/core/data_type.h:156`), so recently acknowledged writes may be lost on
  SIGKILL. The tests assert *consistency and recoverability*, not durability of
  unflushed writes.
- **Empty-graph queries work.** These tests use the `count_vertices()` helper,
  which avoids the label-filtered count defect F2 in
  [08](08-correctness-findings.md).
- **Bulk writes avoid `UNWIND ... CREATE`** because of defect F1.

## 4. What this baseline does not cover

| Not covered | Why |
|---|---|
| Algorithm / procedure integration suites (`test_algo*.py`, `test_sampling.py`, `test_train.py`) | `BUILD_PROCEDURE=OFF` — these are deleted by the CI staging step, matching the upstream unit-test job |
| Fulltext index paths | `ENABLE_FULLTEXT_INDEX` is OFF (the default) |
| HA / Raft behaviour (braft and bolt_raft) | Not exercised by `ut` or the Phase 0 `it` suites; the architecture is documented in [04](04-ha-raft-replication.md) |
| The whole upstream integration suite (`test/integration/*.py`) | Phase 0 ran its own suites plus the upstream unit tests; the full upstream `pytest ./` run was not performed |
| 1000 and 4000 graph measurements | **Covered after the R0 fix** — see `benchmark/scaling/results/R0-VERIFY.md`. Before the fix they were impossible (creation failed at 998) |

The last two are the main open items for the next phase, together with triaging
`TestCypherV2.TestProcedure`.

## 5. R0 fix verification (`MDB_NOTLS`)

The ~998-graph ceiling documented in
[07-scalability-risks.md](07-scalability-risks.md) R0 was fixed by making
`LMDBKvStore` open environments with `MDB_NOTLS` by default (runtime setting
`lmdb_notls`). Consequences for this test baseline:

- **The unit suite now runs entirely under `MDB_NOTLS`.** `unit_test` constructs
  `Galaxy`/`LightningGraph` directly, so it exercises the new default; there is
  no separate configuration to test.
- **Result unchanged from the pre-fix baseline:** 277 passed, 1 failed
  (`TestCypherV2.TestProcedure`), 13 skipped, in the same wall time. The fix does
  not alter test outcomes.
- **A test-only switch** (`--lmdb_notls true|false`, in `test/main.cpp`) was added
  so the suite can be A/B'd against one binary. It was used to establish that the
  intermittent SIGSEGV described in
  [08-correctness-findings.md](08-correctness-findings.md) F3 is **pre-existing**:
  it reproduces at the same rate with the policy off (2/4 runs) as on (3/7 runs),
  at the identical instruction address.
- **Post-fix benchmark** (2000 and 4000 graphs, all phases):
  `benchmark/scaling/results/R0-VERIFY.md` / `r0-verify.json`.

## 6. Summary against the Phase 0 success criteria

| Criterion | Status |
|---|---|
| Clean environment builds TuGraph without undocumented manual intervention | **Met** — `doctor.sh` + `build.sh`, two blockers fixed and documented |
| All existing upstream unit tests pass | **Partially met** — `fma_unit_test` passes; `unit_test` has 1 failure of 291, recorded and not fixed by policy. Additionally ~50% of full-suite runs abort with a pre-existing SIGSEGV ([F3](08-correctness-findings.md)), which reproduces with the storage policy on and off |
| Newly added integration tests pass | **Met** — 33/33 |
| Benchmarks run against 1, 100, 1,000 and 4,000 graphs | **Met** after the R0 fix — 1/100/1000/4000 all complete; 3999/3999 graphs created and verified at 4000 (`results/R0-VERIFY.md`) |
| Startup time, RAM, disk and fd usage recorded for each test size | **Met for achievable sizes** — `benchmark/scaling/results/BASELINE.md` |
| Restart/recovery tests complete without data corruption | **Met** — includes SIGKILL mid-transaction atomicity |
| Architecture document explains graph lifecycle and HA/Raft paths | **Met** — `docs/architecture/01`-`06` |
| Benchmark results reproducible by another developer | **Met in principle** — pinned image ID verified on every run, environment captured with every result, exact commands recorded. Subject to the image-transfer caveat in `ci/phase0/env/README.md` (the image cannot be rebuilt from this checkout) |
