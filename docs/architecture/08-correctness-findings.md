# 08 — Correctness Findings (Phase 0)

Correctness defects discovered while building the Phase 0 test harness. They are
recorded here rather than fixed, because Phase 0 is measurement-only. Both were
found by running ordinary Cypher against the built server
(`lgraph_server` from `ci/phase0/build.sh`), not by static inspection.

Verification environment: arm64 CentOS 7 container, commit `672e4b199`, default
configuration (`is_cypher_v2 = true`).

---

## F1 — `UNWIND ... CREATE` creates only one vertex in the default Cypher v2 engine

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
   not distort throughput numbers — but it does mean `UNWIND`-based ingest
   throughput is **not** measured, and should not be assumed to work.
4. Neither defect is fixed in Phase 0. They are inputs to later phases and should
   be triaged into the platform's issue tracker.

---

## F3 — Intermittent SIGSEGV in the upstream unit test suite (pre-existing)

**Severity: high (roughly half of full unit-test runs abort; CI flakiness).**

Found while attributing a crash seen during the R0 (`MDB_NOTLS`) work. It is
recorded here because it is **not** caused by that change — it reproduces
identically with the storage flag off, which is the pre-change behaviour.

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

