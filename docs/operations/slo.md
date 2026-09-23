# TuGraph HA / Sharding SLOs and Capacity

Service-level objectives and capacity planning for the HA + whole-graph
sharding deployment. Values are targets to validate, not yet measured at
production scale; see `08`/`12` for the semantics they rest on.

## 1. Objectives

| Objective | Target | Basis |
|---|---|---|
| Availability (cluster) | 99.9% | 3-node group tolerates 1 failure |
| Graph availability | 99.9% | placement is single-shard; shard = replica group |
| Failover time (leader loss) | ≤ 5 s p99 | election timeout 0.5–3 s + redirect |
| **RPO** (acknowledged writes) | **0** | commit requires majority before ack (`08 §3`) |
| **RTO** (single replica) | ≤ 60 s | restart + log/snapshot catch-up |
| RTO (single shard) | ≤ 5 min | followers promote within the shard |
| Routing latency (router) | ≤ 1 ms p99 (cache hit) | versioned in-memory cache |
| Stale-routing rejection | 100% | placement-version fence at the shard |
| Migration interruption window | ≤ 5 s | brief quiesce at cutover (Phase 5) |

## 2. Capacity model

Per **open graph** (bounded by `max_open_graphs`), from
`docs/architecture/13-memory-footprint.md`:

| Component | Cost / open graph |
|---|---|
| Refcount arrays | ~60 KiB (×2 objects); ~7.5 KiB with `-DLGRAPH_COMPACT_REFCOUNT=1` |
| LMDB env structures | ~50 KiB resident (lazily faulted) |
| Validator thread | 1 thread |
| fds | ~3 |

Registered graphs beyond the open set cost only a catalog entry
(~150–200 B). The cluster catalog (Phase 4) adds ~24 B/graph.

Measured reference points on the test host:

- **Single replica group**: 3 nodes ≈ 190 MiB.
- **Three shards (9 nodes)**: ≈ 926 MiB total (~308 MiB/shard) — 3-node HA ×3.

Planning rule of thumb:

- **RAM** ≈ `num_shards × per_shard`, where `per_shard ≈ 3 nodes × (60 MiB +
  max_open_graphs × per-open-graph cost)`.
- Scale out by adding shards (each a 3-node group), not by adding graphs per
  shard beyond the open set.

## 3. Recommended starting configuration

| Setting | Value | Why |
|---|---|---|
| `max_open_graphs` | 64–256 per shard | bounds RAM/fds/threads; cold opens are lazy |
| `graph_idle_timeout_s` | 30–900 | reclaim idle graphs |
| `lmdb_max_dbs` | 256 | smaller per-env reservation |
| `ha_election_timeout_ms` | 1000–3000 | prompt failover without flapping |
| `ha_snapshot_interval_s` | 3600 | faster recovery, less I/O |
| `durable` | true | commit to disk (0 RPO) |
| Build | `-DLGRAPH_COMPACT_REFCOUNT=1` | ~8× less refcount RAM |

## 4. Alerting recommendations

- No leader for > 3× election timeout.
- Leader election rate above a threshold (flapping).
- Follower `commit_index` lag exceeding a bound.
- Open-graph count pinned at `max_open_graphs` with eviction-skipped-refs rising.
- Router cache miss rate / stale-rejection rate above baseline.
- Disk utilization per shard above 80%; migration lag and failures.
