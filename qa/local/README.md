# Local QA harness

Pinned-container build, correctness suites, performance profiles and the
scaling benchmark for GraphFin. Everything here runs on a developer machine or
a CI runner with Docker; the repository tree is bind-mounted read-write and no
step modifies upstream source.

`qa/local/run.sh` is the only entry point you need. `entrypoint.sh` runs inside
the container and is invoked by `run.sh`, not by hand.

## Runbook

```bash
# 0. verify the pinned environment is complete and unmodified
qa/local/run.sh doctor

# 1. build lgraph_server and the test binaries into build/output
qa/local/run.sh build                 # clean build, -j2
CLEAN=0 qa/local/run.sh build         # incremental
qa/local/run.sh build image           # build the x86_64 compile image locally

# 2. correctness
qa/local/run.sh ut                    # upstream gtest + fma_unit_test
qa/local/run.sh it                    # integration pytest (perf/soak excluded)
QA_TEST_FILES=test_timeseries.py qa/local/run.sh it
qa/local/run.sh series                # strict series acceptance gate

# 3. performance and soak (require build/output/lgraph_server)
qa/local/run.sh perf smoke            # short profile, a few minutes
qa/local/run.sh perf full             # full-scale series profile
qa/local/run.sh soak 30               # 30-minute mixed-load soak

# 4. scaling benchmark (graph-count measurements, separate harness)
qa/local/run.sh bench --graphs 1 100 1000 4000 \
  --out benchmark/scaling/results/baseline.json
```

`run.sh perf`/`soak` write JSON evidence to `qa-results/`:

| Run | Output | Contents |
|---|---|---|
| `perf smoke` | `qa-results/series-perf-smoke.json` | ingest/read latency, contention, fine-grained cadence, startup/restart, storage amplification, eviction churn |
| `perf full` | `qa-results/series-perf-full.json` | same sections at full workload scale |
| `soak N` | `qa-results/series-perf-soak.json` | per-minute RSS/fd/thread timeline, write/read latency over time, post-soak restart integrity |

Each JSON records the profile, environment, git commit (via `QA_GIT_COMMIT`),
server binary identity, and one section per test. **Performance numbers are
recorded, never gated**: wall-clock on a shared machine is not a contract.
Correctness assertions inside the perf tests (counts, values, eviction
happened) do fail the run.

The soak's duration comes from `QA_SOAK_MINUTES`; `run.sh soak` defaults to 60.
`perf/soak` failures always fail the run. Unlike `ut`/`it`, they are not
suppressed by `QA_TEST_DIAGNOSTIC=1`.

## Where each suite runs

`test/suites.json` is the canonical catalog; runners query it through
`test/suites.py` and never hardcode filters for catalogued suites.

| Suite | Tier | Selection | Invoked by |
|---|---|---|---|
| `pr-unit` | pr | series/shard/router gtest slice | `ci/github_pr.sh` |
| `merge-unit`, `merge-smoke`, `merge-clients`, `merge-ha` | merge | targeted gtest/pytest | `ci/merge/run.py` |
| `main-ut` | main | full gtest, with and without ASan | `ci/github_ci.sh` |
| `main-it` | main | full pytest, marker `not perf and not soak` | `ci/github_ci.sh` |
| `series-gate` | dev | series gtest allowlist | `qa/local/run.sh series` |
| `perf-smoke` | nightly | marker `perf_smoke` | `qa/local/run.sh perf smoke` |
| `perf-series` | nightly | marker `perf` | `qa/local/run.sh perf full` |
| `perf-soak` | nightly | marker `soak` | `qa/local/run.sh soak` |

The `main-it` marker is what keeps measurement code out of the correctness
gate: the perf modules are still collected and import-checked, but not run.

## Environment

`images.lock` pins the compile and runtime images by ID and records the
toolchain and build flags. On arm64 this is the historical CentOS 7 baseline
image used for all committed benchmark results. On x86_64 the compile image is
built from this checkout with `run.sh build image`; its ID is recorded per run
because it is not reproducible byte-for-byte from `images.lock`.

`container.sh` verifies the image ID on every build and test run and aborts on
mismatch, because silently swapping the image would invalidate every previously
recorded number. `QA_SKIP_IMAGE_PIN=1` exists only for CI on a different
architecture; such runs are not comparable to the baseline and record their
actual image ID in the result.

The benchmark and soak scratch databases live in the named Docker volume
`graphfin-qa-data` (mounted at `/data`), not on the repo bind mount: on macOS
the bind mount is virtiofs, whose mmap/fsync behaviour is not representative of
a Linux filesystem and would distort storage, snapshot and restore numbers.
Integration tests use pytest `tmp_path` inside the container instead.

`build-server.sh` defaults to `-j2` because the heavy `cypher/execution_plan`
translation units need 1.5-2.5 GiB of compiler RSS each; on the ~7.8 GiB
baseline VM, `-j4` OOM-kills `cc1plus`. Raise `JOBS` only after raising the
container memory limit.

## Results and logs

- `qa-results/` — pytest/gtest logs, `summary.json`, perf/soak JSON (gitignored)
- `qa-logs/` — build logs (gitignored)
- `benchmark/scaling/results/` — committed scaling baselines

## Historical record

The arm64 Phase 0 baseline, the R0 TLS-ceiling investigation and the archived
experiment/sanitizer sources are under
`docs/architecture/evidence/phase0/`. Those documents describe measurements
taken at the time; commands in them have been updated to the current layout,
but the numbers were produced by the pinned baseline image, not by a later
x86_64 build.

## Known caveats

- `BUILD_PROCEDURE=OFF`, so `learn/` and `procedures/` demo trees are not built
  and the algorithm/procedure integration suites cannot run.
- `ENABLE_FULLTEXT_INDEX` is OFF (the default), so fulltext index paths are not
  exercised.
- The pinned arm64 image was built from a newer upstream Dockerfile revision
  than this checkout and cannot be rebuilt from HEAD; see `env/README.md` for
  the `docker save`/`load` transfer recipe.
- Docker is required for every command in this directory.
