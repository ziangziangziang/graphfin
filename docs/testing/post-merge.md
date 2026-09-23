# Post-merge acceptance plan

Baseline under review: `701fbfb9` on `perf-series-merge` (2026-09-22).
This plan joins the performance and time-series work streams. Results from either
parent branch are regression evidence, not certification of the merged binary.

## Release scope

The current implementation combines native vertex/edge series with lazy graph
loading, bounded open-graph admission, eviction, and existing HA machinery.
Whole-graph placement, routing decisions, fencing and migration state machines
are components. Live cross-shard forwarding, receiver fencing in the server,
replicated placement metadata and migration data movement remain future work.
Do not turn a mocked forwarding test into a claim of a distributed service.

Use three gates: fast merge regression, three-node HA, and dedicated scale/soak.
Each gate fails on missing required cases, skipped required tests, crashes,
timeouts, or missing evidence. A passing smaller gate cannot satisfy a larger one.

## Shared, deterministic fixtures

| Fixture | Graph model | Series | Independent oracle |
| --- | --- | --- | --- |
| Financial dependency | Synthetic issuer, supplier, product, dealer and fund nodes; SUPPLIES, SELLS and HOLDS edges | INT64 scaled price/volume, DOUBLE measurements, edge weights | Hand-calculated observations and a seeded Python reference map |
| Equipment dependency | Machine, sensor, part and site nodes; FEEDS, USES and LOCATED_AT edges | Same types and timestamps as financial fixture | Same reference implementation, different labels/names |

Both fixtures use fixed microsecond timestamps, explicit nulls, sparse samples,
late arrivals, corrections, duplicate timestamps, vertex and edge series.
Tenant graphs deliberately reuse labels, primary keys and timestamps with different
values, exposing graph/store/cache mix-ups. Data is synthetic and offline. No
assertion depends on market behavior, profitability or an external data provider.
Current mutable series cannot reconstruct an earlier knowledge-time view: mark
that future feature explicitly rather than simulating it with current values.

## Test matrix

| ID | Scenario and fault | Required assertions | Gate / implementation |
| --- | --- | --- | --- |
| MERGE-01 | Tenant graphs outnumber the open-graph cap; cold-read vertices and edges repeatedly | Exact schema, timestamps, nulls and values match each tenant oracle; eviction counter increases; open count respects cap | Fast; `test_merge_series.py` |
| MERGE-02 | Correct one measure, issue stale CAS, reject non-finite input; evict and reopen | Unrelated measures survive; rejected writes change no values; fresh reads return corrected state | Fast; `test_merge_series.py` |
| MERGE-03 | Snapshot a quiescent multi-graph fixture through `dbms.takeSnapshot`; restore to an independent directory | Entire vertex and edge series match the oracle, catalog/labels persist; source remains readable | Fast; `test_merge_series.py`; concurrent snapshot added under MERGE-08 |
| MERGE-04 | Delete/recreate a logical graph, reusing its label/field/element IDs | No old buckets or cached values visible; incarnation changes in the control-plane tests | Fast; `test_merge_series.py` plus cluster unit suites |
| MERGE-05 | Parallel writers/readers churn more graphs than the cap | Every acknowledged point appears exactly once in its graph; no cross-tenant values; no lifetime errors; resources eventually return to baseline | Integration; extend workload beyond MERGE-01 |
| MERGE-06 | Hold a real read transaction/iterator at capacity; request another graph, then release | Pinned graph stays usable and snapshot-stable; bounded admission times out explicitly; admission succeeds after release | C++ lifecycle + series integration needed; completed RPC reads alone do not hold a transaction |
| MERGE-07 | Kill process while a mixed graph+series transaction is staged and around commit acknowledgement | Recovered state is entirely before or after transaction; every durable acknowledgement survives; ambiguous requests classified separately | Fault-injection harness needed; idle SIGKILL tests are a baseline only |
| MERGE-08 | Ingest during snapshots; interrupt snapshot/backup and restore with actual tools | Snapshot's declared consistency scope is upheld; incomplete artifacts rejected; restored catalog/schema/series agree; source remains usable | Integration + failure injection; do not assume a global snapshot across independent graph transactions |
| MERGE-09 | Series writes on 3-node HA, follower catch-up, leader SIGKILL, election and rejoin | Full acknowledged vertex/edge observations and metadata eventually equal on every replica; explicit deadlines; no read counted as a write acknowledgement | HA; `test_merge_series_ha.py` baseline |
| MERGE-10 | Minority partition, delayed replay, stale CAS and repeated leaders | No acknowledged minority write; no stale correction overwrites newer data; full per-replica reconciliation after healing | HA chaos extension; test the selected replication path separately |
| MERGE-11 | Whole-graph placement lifecycle with tenant names, recreation and move attempts | UID/epoch fencing, stale/future owner rejection, transaction-local staging, no publication after abort; stale move completion cannot cut over a newer attempt | Fast; cluster/router/migration unit suites, component scope |
| MERGE-12 | Real router + 3 shards × 3 replicas, move/copy/cutover under writes | Physical series bytes and metadata reach only the authoritative owner; stale receiver rejects writes; logical client endpoint survives failover | BLOCKED by missing server integration/data movement; required before distributed-sharding claims |
| MERGE-13 | Denied principal tries series read, write, clear, DDL and export on another tenant | Permission denial before data disclosure/mutation; graph ACL matches ordinary graph access; privileges remain correct after reopen/restore/failover | Client integration needed |
| MERGE-14 | REST, RPC, bundled Python and pinned real Bolt driver | INT64 extremes, microseconds, collection/string distinction, null, finite DOUBLE, error identity and stale CAS survive transport; required driver cannot skip | Existing series suite + new merge fixture; gate dependency provisioning required |
| MERGE-15 | Released pre-series fixture → merge build → add series → restart/backup/restore | Ordinary data remains exact; series survives; unsupported downgrade/mixed-version operation rejected/documented | Release gate; merge-base fixture is not a released-version fixture |
| MERGE-16 | Corrupt/truncated buckets, concurrent decode and eviction | ASan/UBSan/TSan clean on covered paths; bounded allocations; corruption yields controlled error, never partial success | Sanitizer gate; full instrumentation environment still required |
| MERGE-17 | Fresh binary/package install, version output, bilingual quick start and upgrade instructions | Package identity and checksums agree; scripts execute; all local links/assets resolve; attribution retained; no claims beyond evidence | Release packaging gate |

## Performance profiles

Measure with a fixed seed, warm/cold distinction, compiler and durability settings,
host CPU/RAM, container limits and source/binary/image hashes. Write expected data
to an independent oracle before faults; record acknowledgements durably outside
the server process. Check values, not just counts. Report unknown commit outcomes.

| Profile | Size / dimensions | Metrics and acceptance |
| --- | --- | --- |
| Catalog and lazy open | 1k / 10k / 100k registered graphs, small active set; default vs compact refcounts | RSS, mapped/anonymous memory, threads, FDs, startup, admission and eviction; include staging, cache, tombstones and allocator overhead |
| Dense series ingestion | 10M points, sorted/shuffled/descending order | Throughput, retries, p50/p95/p99, peak RSS, bucket bytes and total disk amplification |
| Fine-grained observations | 500 elements × 23,400 one-second samples = 11.7M points | Narrow versus full scans, concurrent correction latency, points/buckets decoded; do not require unavailable bounded APIs to pass |
| Dependency fan-out | Bounded neighborhoods and high-degree hubs; duplicate paths | Exact selected IDs/results, query count, materialized bytes, latency; no cross-shard execution assumed |
| Pressure and retention | Persisted dataset larger than an explicit container memory budget; churn after warm-up | No OOM, bounded open graphs, full data reconciliation, peak/steady RSS, FD/thread leak slope; cancellation latency where supported |
| Replication soak | Three replicas, then three independent replica groups; repeated kills/partitions | 24-hour initial run, 72-hour qualification target; zero lost acknowledged writes or divergent reconciled values; report outage distribution |

Agree and record workload-specific numeric throughput/latency/RSS budgets before
performance sign-off. Do not invent them from small fixtures or put wall-clock
thresholds into unit tests. Nine directly addressed processes prove replica-group
isolation; they do not prove transparent sharding without the real router.

## Build and evidence policy

Build once for a source/configuration snapshot, then reuse its binaries across
gates. Header edits legitimately invalidate dependents; a runtime gtest filter
does not reduce the monolithic test executable's compilation. Keep `JOBS=2` on
the current ~8 GiB Docker VM. Never run concurrent builds in the same build tree.
Documentation-only changes do not justify rebuilding the engine. A future small
series/cluster test target should share compiled library objects.

The merge runner uses isolated scratch databases, preserves logs, parses XML and
records skipped cases. Never clean an existing agent database. The old baseline
runner's always-success exit policy is unsuitable for release acceptance.

Archive: exact commit and dirty-content manifest; build configuration and toolchain;
image ID; hashes of server, libraries, backup tool and tests; commands; XML and logs;
seed/workload and resource limits; fault/acknowledgement ledger; test case coverage;
release artifact checksums. Source changes during a run invalidate certification.
Record `NOT RUN`, `BLOCKED`, `FAILED` and `PASSED` distinctly. Require all selected
release-scope gates before applying a release tag or publishing artifacts.
