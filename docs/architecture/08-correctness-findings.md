# 08 — Correctness Findings (Phase 0)

Correctness defects discovered while building the Phase 0 test harness. They were
originally recorded here rather than fixed, because Phase 0 is measurement-only.
Both were found by running ordinary Cypher against the built server
(`lgraph_server` from `ci/phase0/build.sh`), not by static inspection.

> **Update (Phase 2 validation):** F1 and F2 have since been fixed, together
> with two crashes introduced during the scaling work — F3a (eviction task
> use-after-free) and F4 (online snapshot under lazy loading). Each finding
> below carries a STATUS note. Only F3 (pre-existing subprocess-teardown
> corruption) remains open.

Verification environment: arm64 CentOS 7 container, commit `672e4b199`, default
configuration (`is_cypher_v2 = true`).

---

## F1 — `UNWIND ... CREATE` creates only one vertex in the default Cypher v2 engine

**STATUS: FIXED.** Per-record `Visited` reset in `OpGqlCreate::CreateVE`
(`src/cypher/execution_plan/ops/op_gql_create.h`) and `OpCreate::CreateVE`
(`src/cypher/execution_plan/ops/op_create.h`): each input record clears the
created-node flags before creating, so the create runs once per row. Verified
by empirical repro and the upstream `TestCypherV2` suite.

**Severity: critical (silent data loss / wrong results in bulk writes).**

### Reproduction

```cypher
CALL db.createVertexLabel('person','id','id','INT64',false,'name','string',true);
UNWIND range(1,5) AS i CREATE (n:person {id:i, name:'u'});
MATCH (n) RETURN count(n);
```

Observed:

```
UNWIND..CREATE   ok=True raw=[{"<SUMMARY>":"created 1 vertices, created 0 edges."}]
count            ok=True raw=[{"count(n)":1}]
UNWIND..RETURN i ok=True raw=[{"i":1},{"i":2},{"i":3},{"i":4},{"i":5}]
```

The `UNWIND` produces 5 rows correctly (`RETURN i` returns 1..5, and
`RETURN count(i)` returns 5), but the following `CREATE` executes **once**.
Five rows of input produce one vertex.

### It is specific to the default Cypher v2 path

Same statement with the engine switched:

| `is_cypher_v2` | Result |
|---|---|
| `true` (**default**, `src/core/global_config.h:64`) | `created 1 vertices` — **wrong** |
| `false` | `created 5 vertices` — correct |

The flag is registered at `src/core/global_config.cpp:353-354` as
`--is_cypher_v2` and defaults to `true`, so **the default configuration of
TuGraph 4.5.2 is affected**.

### Correct bulk-write formulation

A single statement with a comma-separated CREATE list works and is one
transaction:

```cypher
CREATE (a:person {id:1,name:'a'}), (b:person {id:2,name:'b'}), (c:person {id:3,name:'c'});
-- -> "created 3 vertices"
```

### Impact on this project

Any bulk-load path that relies on `UNWIND ... CREATE` silently under-inserts.
For a platform expected to host many graphs with data ingestion workloads this
is a correctness blocker, and it also means test suites that bulk-load via
`UNWIND` have been asserting against under-populated graphs without noticing.

### Where to look

The Cypher v2 execution path: `src/cypher/execution_plan/execution_plan_v2.cpp`,
`src/cypher/execution_plan/ops/` (the create operator), and the v2 clause
ordering in `src/cypher/execution_plan/scheduler.cpp:167-268` (`EvalCypher2`).
The v1 path (`EvalCypher`, `:69-151`) behaves correctly, which localizes the
defect to the v2 plan/operator implementation rather than the parser or the
storage layer.

---

## F2 — Label-filtered `count()` fails on an empty label

**STATUS: FIXED.** Guarded the empty-graph case in
`lgraph_api::traversal::FindVertices` (`src/lgraph_api/lgraph_traversal.cpp`),
which constructs a zero-capacity `ParallelVector` and threw
"capacity cannot be 0"; and made the count traversal operator emit a single
`0` row when the result set is empty (`OpGqlTraversal`/`OpTraversal`,
`src/cypher/execution_plan/ops/op_gql_traversal.h` /
`src/cypher/execution_plan/ops/op_traversal.h`).

**Severity: high (breaks the common "is this graph empty?" query; directly hits
multi-graph workloads where most graphs start empty).**

### Reproduction

On a graph where the label is declared but has zero vertices:

```cypher
MATCH (n:person) RETURN count(n);
```

Observed:

```
ok=False raw=capacity cannot be 0
```

After at least one vertex exists the same query succeeds:

```
MATCH (n:person) RETURN count(n)  ->  ok=True raw=[{"count(n)":1}]
```

An unlabelled count works in both cases:

```
MATCH (n) RETURN count(n)  ->  ok=True    (0 on an empty graph)
```

### Impact on this project

Multi-graph deployments have many graphs that are empty or whose labels are
empty. Any monitoring, health-check or application query of the form
`MATCH (n:Label) RETURN count(n)` fails on exactly those graphs. The failure is
an error, not a wrong number, so it is at least loud — but it makes the most
natural emptiness check unusable.

### Workaround used by the Phase 0 tests

`count_vertices()` in `test/integration/phase0_util.py` tries the
label-filtered count and falls back to counting returned rows via
`MATCH (n:Label) RETURN n`, which does not hit the faulty path.

### Where to look

The count/aggregate fast path for label scans. Candidates:
`src/cypher/execution_plan/ops/` (the aggregate / label-scan operators) and the
vertex-index "ready" handling (`src/core/index_manager.cpp:47-64`,
`src/core/vertex_index.h`). The message "capacity cannot be 0" suggests a
container or table being sized from the live row count and then rejected when
that count is zero.

---

## How these were found, and what it means for the baseline

Both were found by empirically probing ordinary Cypher rather than by reading
code: the Phase 0 integration suite initially failed in ways that looked like
test bugs, and narrowing them down produced these two defects. That is the
intended outcome of a baseline phase — the harness is now the thing that
detects them.

Consequences carried into the rest of Phase 0:

1. The Phase 0 test suite avoids `UNWIND ... CREATE` and uses comma-separated
   CREATE lists for bulk writes (`test_restart_and_failure_recovery.py`).
2. The Phase 0 test suite uses `count_vertices()`, which tolerates empty labels.
3. Benchmark write workloads use one `CREATE` per request, so the F1 defect does
   not distort throughput numbers — but it means `UNWIND`-based ingest
   throughput was **not** measured, and should not be assumed to work.
4. F1 and F2 were **not fixed in Phase 0** and were carried as findings. They
   were resolved during the Phase 2 validation work, together with the
   Phase 2-introduced F3a crash (see the STATUS notes on each finding above).
   F3 (pre-existing subprocess-teardown corruption) remains open.

---

## F3 — Intermittent SIGSEGV in the upstream unit test suite (pre-existing)

**Severity: high (roughly half of full unit-test runs abort; CI flakiness).**

Found while attributing a crash seen during the R0 (`MDB_NOTLS`) work. It is
recorded here because it is **not** caused by that change — it reproduces
identically with the storage flag off, which is the pre-change behaviour.

> **Note:** the Phase 2 work exposed a *second*, distinct SIGSEGV in the
> eviction task (see F3a below). That one **has been fixed**. What remains —
> and what this document originally described — is an independent memory
> corruption that clusters on tests with subprocess teardown.

### Evidence

Measured across two arms that differ only in the `lmdb_notls` policy, running
the full `unit_test` suite (291 tests) repeatedly in the pinned container:

| Arm | Valid runs | SIGSEGV | Crash rate |
|---|---|---|---|
| `lmdb_notls = true` (new default) | 7 | 3 | 43% |
| `lmdb_notls = false` (pre-change behaviour) | 4 | 2 | 50% |

The control arm sets the flag off for **both** the test process and the
`lgraph_server` subprocesses the tests spawn (`"lmdb_notls": false` in
`lgraph_server.standalone.json`), so it is behaviourally equivalent to the code
before the R0 change.

Every crash lands at the **identical instruction address**:

```
Program terminated with signal 11, Segmentation fault.
#0  0x0000000001182e0c in
    std::__cxx11::basic_string<char,...>::_M_assign (...)
    at /usr/local/include/c++/8.4.0/bits/basic_string.tcc:254
```

Runs that do not crash produce exactly the baseline result (277 passed, 1
failed — the F1-adjacent `TestCypherV2.TestProcedure` golden mismatch). So the
suite is either fully normal or dies, with no middle ground.

### Crash sites

| Site | When | Observed |
|---|---|---|
| `TestBackupRestore.BackupRestore` | ~6–7 s into the run | Most common. This test **spawns `lgraph_server` subprocesses** (`lgraph_import`, `lgraph_binlog`) via `tiny-process-library` (`test/test_backup_restore.cpp:40-58,67-70,99-103,114-118`) |
| `TestCypherV2.TestProcedure` | ~250 s into the run | Seen once |

Crashing inside `std::string::_M_assign` means memory corruption — an invalid
`this` or source string — rather than a clean logic error.

### Not yet determined

- The caller. Core files are written to the virtiofs bind mount and are
  **truncated** (e.g. 18 MB of an expected 522 MB), so `gdb` can only read frame
  #0. Getting a full stack needs the binaries copied to container-local storage
  first, or `gdb` catching the signal live (which perturbs timing and did not
  reproduce the crash in one attempt).
- Whether the defect is in the product or in the test harness. The clustering on
  the one test that manages subprocesses points at `tiny-process-library` /
  `SubProcess` teardown, but that is a hypothesis, not a finding.

### Why it matters for Phase 1

Roughly half of full unit-test runs abort, so the unit suite **cannot currently
be used as a clean pass/fail gate**. Fixing or quarantining F3 should precede
relying on it to validate the graph-count work.

### Reproduction

```bash
# A/B harness (test-only --lmdb_notls switch, see test/main.cpp)
ci/phase0/experiments/run_notls_matrix.sh on 4
ci/phase0/experiments/run_notls_matrix.sh off 4
```

### Status update (Phase 4 review) — still open, diagnosis blocked in this window

The reviewer flagged this as an open reliability gap (`REVIEW.md` finding: "make
test failures propagate … diagnose crashes"). It remains the one defect that
prevents the upstream unit suite from being a clean pass/fail gate.

Diagnosis could not be completed in the allotted time box; the blocker is
environmental, not analytical:

- Core files written to the virtiofs bind mount are **truncated** (frame #0 only),
  so the caller cannot be read. A full stack needs cores written to
  container-local storage, or `gdb` catching the signal live.
- The crash **clusters on a test that spawns `lgraph_server` subprocesses**
  (`TestBackupRestore`), pointing at `tiny-process-library` teardown, but this is
  a hypothesis.

Concrete next steps (unblocked, ~1–2 h):
1. Run `unit_test --gtest_filter=TestBackupRestore.*` in a loop (say 50×) inside
   the container with `ulimit -c unlimited` and cores written to `/tmp` (not the
   bind mount); inspect the full stack of the first crash.
2. Build the suite with `-DENABLE_ASAN=ON` and run `TestBackupRestore` alone; ASAN
   will name the corrupting write for the subprocess-teardown path.
3. If confirmed in the harness, quarantine by marking the subprocess-teardown
   tests (or the F3 interaction) `GTEST_SKIP` behind an env flag while a fix is
   developed, and keep the suite a reliable gate.

---

## F3a — Phase 2 eviction task use-after-free (crash inside `EvictIdleGraphs`)

**Status: FIXED.** This defect was introduced by the Phase 2 lazy-eviction
work (`feat(graph): lazy graph loading with LRU eviction`, commit `250881908`)
and was the dominant unit-suite crash while both F3 and F3a were present.

**Root cause.** `StartEvictionTask` registered a recurring task on the
**process-global** `TimedTaskScheduler` whose lambda captured `[this]` — a raw
`GraphManager*`. `Galaxy` replaces `graphs_` with a copy-on-write copy in
`CreateGraph`/`DeleteGraph`/`ModGraph` (`src/db/galaxy.cpp`), and destroys it
in `~Galaxy`, `LoadSnapshot`, and `ReloadFromDisk` — but none of those paths
cancelled the task, so after the first graph create/delete/mod the scheduler
fired `EvictIdleGraphs()` into freed memory. Because the task is also a
recurring one that copies its function locally before running, `Cancel()`
alone could not stop an already-started invocation.

**Evidence (gdb backtrace, live run of the full unit suite):**

```
Program received signal SIGSEGV
#0 load (__m=std::memory_order_acquire, this=0x732e3830303030a0)   // garbage = ASCII "s.800000"
    at /workspace/include/fma-common/rw_lock.h:125
#1 AtomicLoad<long> ...
#2 fma_common::InterruptableTLSRWLock<...>::WriteLock (this=0xffff7d7465a0) at rw_lock.h:135
#5 lgraph::GraphManager::EvictIdleGraphs (this=0xffff7d7464c0) at src/db/graph_manager.cpp:379
#7 fma_common::RecurringTask::Run ...        // the idle-eviction background task
```

The `0x732e3830303030a0` pointer decodes to the ASCII bytes `s.800000` — a
`std::string` had reused the freed `lock_` memory, i.e. the manager was gone.

**Fix.** The eviction task is now owned by the stable `Galaxy` (never copied)
and resolves the current `GraphManager` through `graphs_lock_` on every tick:

- `Galaxy::StopEvictionTask` cancels the task and drains any in-flight run by
  briefly taking `graphs_lock_` write (an in-flight `EvictIdleGraphsTick` holds
  `graphs_lock_` read, so this blocks until it finishes).
- `Galaxy::EvictIdleGraphsTick` re-reads `graphs_` under `graphs_lock_` read
  and, crucially, options the whole eviction under that lock so a concurrent
  copy-on-write can never destroy the manager mid-eviction.
- `StopEvictionTask` is called from `~Galaxy`, `ReloadFromDisk` and
  `LoadSnapshot`; the COW paths need no per-operation cancel because the
  callback resolves the manager identity at tick time.

Files: `src/db/galaxy.h`, `src/db/galaxy.cpp`, `src/db/graph_manager.{h,cpp}`.

**Verification.** A full `unit_test` run on the F3a-fixed build completed all
291 tests without a SIGSEGV (previously ~50% of runs aborted with `exit 139`);
the remaining hangs/crashes in the suite are the pre-existing F3 corruption
above, which reproduces on an unmodified baseline.

---

## F4 — `CALL dbms.takeSnapshot()` fails with "Nested transaction is forbidden" (Phase 2 lazy-loading regression)

**Status: FIXED.**

**Root cause.** `StateMachine::TakeSnapshot` → `Galaxy::SaveSnapshot` →
`GraphManager::Backup` cold-opens graphs via `GetOrOpenGraphRef`. Each
cold-open constructs a `LightningGraph`, whose `PluginManager` constructor
creates a **write transaction** (`plugin_manager.cpp:45` →
`LightningGraph::CreateWriteTxn` → `Transaction::EnterTxn`). When snapshot is
invoked from the Cypher procedure `dbms.takeSnapshot()`, the request's own
`ctx->txn_` is still open, so `EnterTxn` throws
"Nested transaction is forbidden".

This was not a problem in Phase 0/1: before lazy loading, every graph was
physically open at server start, so `Backup` never cold-opened a graph and no
nested transaction was created. The Phase 2 benchmark predates the fix and,
notably, `phase2-100k.json` was run with `snapshot` excluded from its phase
list, so the failure was not caught.

**Evidence (live gdb backtrace, `catch throw`):**

```
#1 lgraph::Transaction::EnterTxn (transaction.cpp:121)  // nested txn guard
#3 lgraph::LightningGraph::CreateWriteTxn (lightning_graph.cpp:38)
#4 SingleLanguagePluginManager ctor (plugin_manager.cpp:45)
#5 PluginManager ctor (plugin_manager.cpp:737)
#6 LightningGraph::Open (lightning_graph.cpp:3074)
#7 LightningGraph ctor
#8 GraphManager::OpenGraphInternal (graph_manager.cpp:355)
#9 GraphManager::GetOrOpenGraphRef
#10 GraphManager::Backup
#11 Galaxy::SaveSnapshot (galaxy.cpp:579)
#12 StateMachine::TakeSnapshot
#14 BuiltinProcedure::DbmsTakeSnapshot (procedure.cpp:2382)
```

**Fix.** `DbmsTakeSnapshot` now aborts the request transaction before taking
the snapshot, matching the existing pattern in `DbmsMetaRefreshCount`
(`procedure.cpp`). `Transaction::Commit()` already guards `!IsValid()`, so an
aborted request txn is safe to leave uncommitted. The snapshot itself is
whole-server and does not depend on the request txn. Verified by restarting
the online snapshot in the Phase 0 benchmark at 4000 graphs (previously
failing) and by direct repro at 200 and 4000 graphs.

---

