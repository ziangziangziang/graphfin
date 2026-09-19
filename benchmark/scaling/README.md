# Phase 0 graph-count scaling benchmark

Measures how TuGraph behaves as the number of graphs grows. Established for
Phase 0 of the TuGraph scaling project and intended as the baseline every later
phase is compared against.

## What it measures

For each graph count N (default `1, 100, 1000, 4000`) the harness runs one full
cycle:

| # | Phase | Metric |
|---|---|---|
| 1 | fresh db dir | — |
| 2 | `startup` | seconds from process spawn to the `"Server started."` log line |
| 3 | `create` | per-graph create latency, bucketed by how many graphs already existed |
| 4 | `steady_metrics` | RSS, VmSize, thread count, fd count, mapping count |
| 5 | `storage` | on-disk bytes per empty graph |
| 6 | `first_query` | cold first-query latency (default graph and the last graph) |
| 7 | `repeated_query` | p50/p95/p99 over N queries spread across graphs |
| 8 | `throughput` | reads/s and writes/s over a fixed duration |
| 9 | `snapshot` | `dbms.takeSnapshot()` duration, size, file count |
| 10 | `shutdown` | SIGTERM → process exit |
| 11 | `restart` | **startup again with N graphs already on disk** — the key scaling number |
| 12 | `restore` | snapshot copy + startup + verification in a fresh dir |
| 13 | `delete` | per-graph delete latency, bucketed |

Every phase is timed and error-isolated. A failure is recorded together with the
resource peaks observed at the moment of failure, and the JSON is flushed after
every phase, so a long run is never lost and a failure is itself a result.

## Why the O(N) create bucket table matters

`Galaxy::CreateGraph` copy-constructs the entire `GraphManager` **and**
`AclManager` under a global write lock (`src/db/galaxy.cpp:192-193`). With one
user this is O(N) per create and therefore O(N²) to create N graphs. The
bucketed latency table makes that visible from a single run without needing a
separate experiment per graph count.

## Running

All runs happen inside the pinned arm64 container. Scratch databases live in a
named Docker volume (`tugraph-phase0-data`), **not** in the repo bind mount:
on macOS the bind mount is virtiofs, whose mmap/fsync behaviour is not
representative of a Linux filesystem and would distort storage, snapshot and
restore numbers.

```bash
# prerequisites
ci/phase0/doctor.sh
ci/phase0/build.sh

# smoke run (fast, good for validating the harness)
ci/phase0/run_bench.sh --quick --graphs 1 100

# full baseline with explicit, reproducible resource limits
PHASE0_DOCKER_EXTRA='--cpus=4 --memory=6g' \
  ci/phase0/run_bench.sh --graphs 1 100 1000 4000 \
    --out benchmark/scaling/results/baseline.json

# render the markdown report
python3 benchmark/scaling/report.py benchmark/scaling/results/baseline.json
```

The scripts also run directly inside the container / devcontainer:

```bash
python3 benchmark/scaling/run_bench.py --graphs 1 100 --quick
python3 benchmark/scaling/envprobe.py
```

## Important options

| Option | Default | Notes |
|---|---|---|
| `--graphs` | `1 100 1000 4000` | 10,000 verified (`results/PHASE1-10K.md`); the graph limit is now configurable via `max_graphs` |
| `--max-size-gb` | `1` | **Do not omit.** The engine default is 4 TiB per graph (`src/core/defs.h:154`); 1000+ graphs at that size exhaust virtual address space |
| `--repeated-queries` | 1000 | latency sample size |
| `--throughput-seconds` | 10 | duration of the concurrent read/write phase |
| `--throughput-threads` | 4 | reader threads; writers = threads/2 |
| `--phases` | all | run a subset, e.g. `--phases start create restart stop` |
| `--reuse-db` | off | keep and reuse an existing db dir |
| `--keep` | off | keep db/snapshot dirs afterwards |
| `--quick` | off | shrink query/throughput phases for harness validation |

## Resource limits and what will break first

> **Historical note: graph creation used to fail at 998 graphs**, because each
> graph is its own LMDB environment and the default build did not pass
> `MDB_NOTLS`, so LMDB allocated one pthread TLS key per graph and hit glibc's
> `PTHREAD_KEYS_MAX = 1024`. That ceiling has since been **removed** (the engine
> sets `MDB_NOTLS` by default, configurable via `lmdb_notls`), and 2000- and
> 4000-graph runs now complete every phase. See
> `docs/architecture/07-scalability-risks.md` R0 for the analysis and
> `results/R0-VERIFY.md` for the post-fix numbers.

Per graph the process needs roughly **1 thread** (LMDB validator,
`src/core/lmdb_store.cpp:105`), **~3 file descriptors** measured, **~4.4 memory
mappings**, and `db_size` of virtual address space. At the measured ceiling of
998 graphs: ~1,074 threads, ~3,015 fds, ~4,367 mappings, ~1.9 GiB RSS and
~5.1 TiB of virtual address space (dominated by the 4 TiB default mmap of the
auto-created `default` graph).

Run with explicit limits so results are comparable, and make sure the container
allows enough of each:

```bash
PHASE0_DOCKER_EXTRA='--cpus=4 --memory=6g --pids-limit=8192' \
  ci/phase0/run_bench.sh --graphs 1 100 1000 4000
```

`ci/phase0/container.sh` already sets `--ulimit nofile=1048576:1048576`. The
harness records `vm.max_map_count`, cgroup pids/memory/cpu limits and the image
ID with every result, and reports the exact graph index at which a create or
delete failed — which is how the 998-graph ceiling was found.

## Dependencies

None beyond the Python 3.6 standard library that ships in the pinned image.
`requirements.txt` is intentionally empty of runtime dependencies. Everything
(`/proc` sampling, HTTP, JSON, statistics) is stdlib so the harness cannot be
broken by a package mirror.

## Output

- `results/baseline.json` — full machine-readable result (env + per-N runs)
- `results/BASELINE.md` — generated report (`report.py`)

Both are committed so later phases can diff against them. Re-running with a
different image ID, CPU count or memory limit invalidates comparability — the
environment block in the JSON is what makes the numbers interpretable.

## Caveats

- The harness measures a **production-like** server: REST and RPC enabled, Bolt
  enabled, plugin loading on, authentication on. A 1-graph run establishes the
  constant (non-per-graph) overhead.
- `snapshot` uses the online Cypher procedure. The offline `lgraph_backup` tool
  takes an exclusive Galaxy write lock (`src/db/galaxy.cpp:591`) and is not
  exercised here; full backup is whole-server and O(N) regardless.
- Throughput runs target the `default` graph only, so the number measures query
  throughput, not aggregate throughput across graphs.
- `first_query` is "cold" only in the sense of an empty plan cache and cold LMDB
  pages; all graphs are already open because startup opens every graph eagerly.
