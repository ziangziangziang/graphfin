# Phase 0 — Reproducible build, tests and graph-count baseline

This directory contains the Phase 0 tooling for the TuGraph scaling project.
The goal of Phase 0 is a trustworthy baseline: a reproducible build, documented
architecture, automated correctness tests, and graph-count measurements that a
later phase can diff against. **Phase 0 makes no engine behavior changes.**

## Deliverables and where they live

| Deliverable | Location |
|---|---|
| Pinned environment | `ci/phase0/images.lock`, `ci/phase0/env/` |
| Preflight check | `ci/phase0/doctor.sh` |
| Containerized build | `ci/phase0/build.sh` |
| Test runner | `ci/phase0/run_tests.sh`, `ci/phase0/run_tests_inner.sh` |
| Benchmark driver | `ci/phase0/run_bench.sh` → `benchmark/scaling/` |
| Shared container config | `ci/phase0/container.sh` |
| Integration + failure tests | `test/integration/test_multi_graph_*.py`, `test_restart_and_failure_recovery.py`, `test_backup_restore_scale.py`, `test/integration/phase0_util.py` |
| Benchmark harness | `benchmark/scaling/` |
| Baseline results | `benchmark/scaling/results/` |
| Architecture report | `docs/architecture/` |
| Risk register and components to change | `docs/architecture/07-scalability-risks.md` |
| CI workflow | `.github/workflows/phase0-baseline.yml` |
| Dev environment | `.devcontainer/devcontainer.json` |

## Runbook

```bash
# 0. verify the pinned environment is complete and unmodified
ci/phase0/doctor.sh

# 1. build TuGraph (clean build; ~1h on a 10-core / 8 GiB Docker VM)
ci/phase0/build.sh

# 2. upstream unit tests (failures are documented, not fixed, in Phase 0)
ci/phase0/run_tests.sh ut

# 3. integration + failure tests, including the new Phase 0 suites
ci/phase0/run_tests.sh it
PHASE0_TEST_FILES=test_multi_graph_lifecycle.py ci/phase0/run_tests.sh it

# 4. benchmark, then render the report
#    NOTE: graph creation fails at ~998 graphs (pthread TLS key exhaustion,
#    docs/architecture/07-scalability-risks.md R0), so the 1000/4000 runs stop
#    there by construction. That is the headline Phase 0 result, not a harness bug.
PHASE0_DOCKER_EXTRA='--cpus=4 --memory=6g' \
  ci/phase0/run_bench.sh --graphs 1 100 1000 4000 \
    --out benchmark/scaling/results/baseline.json
python3 benchmark/scaling/report.py benchmark/scaling/results/baseline.json
```

Results land in `phase0-results/` (test summaries and logs) and
`benchmark/scaling/results/` (benchmark JSON and report). Both are gitignored
except the benchmark results, which are committed so later phases can diff.

## Design notes

**One source of truth for the environment.** `ci/phase0/images.lock` pins the
compile and runtime images by ID, lists every required tool and library, and
records the build flags. `container.sh` verifies the image ID on every build and
benchmark run and aborts on mismatch, because silently swapping the image would
invalidate every previously recorded number. `PHASE0_SKIP_IMAGE_PIN=1` exists
only for CI on a different architecture; such runs are not comparable to the
baseline and record their actual image ID in the result.

**Architecture-aware build.** `build.sh` detects aarch64 and passes
`-DENABLE_BUILD_ON_AARCH64=ON`, without which `deps/geax-front-end` applies the
x86-only `-msse4.2` flag and the build dies. See
`env/Dockerfile.phase0` for the Cython 3.0 requirement.

**Benchmark scratch space is not on the bind mount.** Benchmark databases live
in the named Docker volume `tugraph-phase0-data` (mounted at `/data`). On macOS
the repo bind mount is virtiofs, whose mmap/fsync behaviour is not
representative of a Linux filesystem and would distort storage, snapshot and
restore numbers.

**Failures are results.** `run_bench.py` times and error-isolates every phase,
records the resource peaks observed at the moment of failure, and flushes the
JSON after each phase. The 4000-graph run is expected to surface limits; those
limits are the deliverable, not a reason to abort.

**No upstream source changes.** Phase 0 modifies only `.gitignore` (to ignore
`phase0-logs/` and `phase0-results/`) and adds new files. Everything else is
build flags, container layers and test/benchmark code. Verify with
`git status --short`.

## Phase 0 results at a glance

Recorded on the pinned arm64 image with `--cpus=4 --memory=6g`
(`benchmark/scaling/results/BASELINE.md`).

| Item | Result |
|---|---|
| **Max graphs openable** | **10,000 verified working** (was 998 before the R0 fix; configurable via `max_graphs`, 0 = unlimited) |
| Graph creation past 998 | **works** after the R0 fix; 9999/9999 created in 18.6 s and the server restarted with them in 9.5 s (`results/PHASE1-10K.md`) |
| Per-graph cost (measured) | ~1 thread, ~3 fds, ~4.4 mappings, ~111 KiB disk, ~1.9 MiB RSS |
| Startup, 1 → 998 graphs | 0.09 s → ~0.05 s (empty graphs open cheaply) |
| Restart with graphs on disk, 1 → 998 | 0.15 s → 0.40 s (≈0.4 ms per graph — O(N) but small for empty graphs) |
| Shutdown, 998 graphs | 0.82 s |
| Threads / fds / mappings at 10,000 | 10.1k / 30.0k / 41.1k |
| RSS at 10,000 | 6.6 GiB (~0.7 MiB per graph — the cost falls with scale) |
| Virtual address space | dominated by the 4 TiB default mmap of `default`; see `results/PHASE1-10K.md` |
| `MAX_NUM_GRAPHS` | 4096 documented, **unreachable** |

**Two correctness defects were also found** and are documented but not fixed:
`UNWIND ... CREATE` creates only one vertex in the default Cypher v2 engine, and
a label-filtered `count()` fails on an empty label. See
`docs/architecture/08-correctness-findings.md`.

### The R0 fix, and how to reproduce the reasoning

Graph creation used to fail at 998 graphs: a pthread TLS-key limit in LMDB, not
a TuGraph graph-count limit. The engine now sets `MDB_NOTLS` by default
(configurable via `lmdb_notls`), which removes that ceiling.

The underlying mechanism is still demonstrable with:

```bash
ci/phase0/experiments/run_lmdb_tls_limit.sh
# TLS    FAILED opening environment #1025: rc=11 (Resource temporarily unavailable)
# NOTLS  opened 5000 environments without failure
```

and the post-fix behaviour is measured in
`benchmark/scaling/results/R0-VERIFY.md` (2000 and 4000 graphs, all phases).

To A/B the storage policy against the unit suite:

```bash
ci/phase0/experiments/run_notls_matrix.sh on 4
ci/phase0/experiments/run_notls_matrix.sh off 4
``````

Upstream test baseline (build, unit tests, integration tests, and the one
upstream failure) is recorded in `docs/architecture/09-test-baseline.md`.

## Known environment caveats

- The pinned base image was built from a newer upstream Dockerfile revision than
  this checkout, so `docker build` does not reproduce it. Phase 0 pins by ID and
  provides a `docker save`/`load` transfer recipe in `env/README.md`.
- `BUILD_PROCEDURE=OFF` (matching the upstream unit-test CI job), so the
  `learn/` and `procedures/` demo trees are not built and the algorithm/procedure
  integration suites cannot run.
- `ENABLE_FULLTEXT_INDEX` is OFF (the default), so fulltext index paths are not
  exercised.
- The Docker VM on the baseline host has ~7.8 GiB RAM and 10 CPUs. A build at
  `-j4` peaks near the memory limit and uses swap; higher parallelism risks
  OOM-killed compilers.
