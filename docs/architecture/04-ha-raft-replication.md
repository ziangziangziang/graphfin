# 04 — HA, Raft Replication and Recovery

## 0. There are two independent Raft subsystems

This is the single most important thing to understand about TuGraph HA. The tree
contains **two unrelated replication implementations that share no state and can
run at the same time**:

| | Legacy HA (braft) | Bolt HA |
|---|---|---|
| Enabled by | `enable_ha=true` | `bolt_port>0` **and** `bolt_raft_port>0` |
| Library | Apache braft / brpc (installed in `/usr/local`) | vendored `deps/etcd-raft-cpp` |
| State machine | `lgraph::HaStateMachine` | `lgraph::StateMachine` driven by `bolt::ApplyRaftRequest` |
| Group id | constant string `"lgraph"` | one unnamed global group per process |
| Replicated unit | a whole `LGraphRequest` (any write op) | a raw Bolt `Run` message (Cypher text) |
| Main files | `src/server/ha_state_machine.{h,cpp}`, `src/server/state_machine.cpp` | `src/bolt_raft/*`, `src/server/bolt_raft_server.cpp`, `src/server/bolt_handler.cpp` |
| Snapshotting | real braft snapshots | **deliberately disabled** |

`LGraphServer::Start` selects `HaStateMachine` when `enable_ha`
(`src/server/lgraph_server.cpp:240-246`) and **independently** starts
`BoltRaftServer` when `bolt_raft_port > 0` (`:327-362`). Both can be active.

The vendored etcd-raft port is pinned at commit
`f1c02c9909c0db10ebb9ed81483254139aa7b881`
(`deps/etcd-raft-cpp/commit`).

## 1. Topology: one global Raft group per process

### Bolt HA

`bolt_raft::BoltRaftServer` is a process-wide singleton holding exactly one
`RaftDriver` (`src/server/bolt_raft_server.h:26-50`), and `RaftDriver` owns
exactly one `eraft::RawNode` (`src/bolt_raft/raft_driver.h:143`). It is created
once at process start:

```
LGraphServer::Start                      src/server/lgraph_server.cpp:356-361
  └─ BoltRaftServer::Instance().Start()  src/server/bolt_raft_server.cpp:29-82
       └─ raft_driver_ = make_unique<RaftDriver>(...)   :54-56
            └─ RaftDriver::Run()         src/bolt_raft/raft_driver.cpp:249-339
                 └─ eraft::NewRawNode(config)           :299
```

**N graphs does not create N Raft nodes.** There is exactly one Raft instance per
process regardless of graph count.

### Legacy HA (braft)

Exactly one `braft::Node` per process, with the group id hard-coded to the string
`"lgraph"` (`src/server/ha_state_machine.cpp:80-81`, also `:132`, `:657`).

## 2. What is replicated

### Bolt HA — the raw Cypher statement

- `RaftRequest{id, user, raw_data}` where `raw_data` is the Bolt `Run` payload
  (`src/bolt_raft/bolt_raft.proto:18-22`).
- Read-only statements are filtered out before proposing; only writes are
  replicated (`src/server/bolt_handler.cpp:282-299`).
- On apply, **every node re-parses and re-executes the Cypher text locally**
  (`src/server/bolt_handler.cpp:115-163`).
- Graph identity is just a string inside the Cypher (`extra["db"]`,
  `src/server/bolt_handler.cpp:255-258`). Raft has no concept of graphs.

### Legacy HA — the whole request

- The entire serialized `LGraphRequest` proto is put into the braft log
  (`HaStateMachine::ReplicateAndApplyRequest`,
  `src/server/ha_state_machine.cpp:224-245`).
- Applied on every node via `on_apply`, with per-index dedup against the metadb
  (`src/server/ha_state_machine.cpp:325-382`; `iter.index() > committed_index`
  at `:368`).
- Because graph create/delete are ordinary write requests, they are replicated by
  this path: `on_apply` → `ApplyRequestDirectly` → `ApplyGraphRequest`
  (`src/server/state_machine.cpp:564-591`).

## 3. Thread and timer footprint

### Bolt HA (`RaftDriver::Run`)

Exactly **four threads**, named for observability
(`src/bolt_raft/raft_driver.cpp:318-337`): `raft_service`, `timer_service`,
`apply_service`, `client_service`. Plus:

- `tick_timer_` — the Raft logical clock, default 100 ms
  (`src/bolt_raft/raft_driver.h:139-140`; loop `raft_driver.cpp:388-401`)
- `compact_timer_` — log GC, default 10 minutes
  (`src/bolt_raft/raft_driver.h:141-142`; loop `raft_driver.cpp:450-471`)

Network side: `BoltRaftServer::Start` runs one listener thread named
`raft_listener` (`src/server/bolt_raft_server.cpp:35,72-73`) and an
`IOServicePool(1)` adds one `io-worker-0` thread
(`src/bolt_raft/io_service.h:34-53,97-107`). Each peer gets a `NodeClient` with a
reconnect timer but **no dedicated thread** — peers are multiplexed onto the
shared `client_service_` thread (`src/bolt_raft/raft_driver.cpp:278-283`).

**Total: ~6 threads + 2 timers per process, flat in N graphs.**

### Legacy HA (braft)

Adds one `heartbeat_thread_` (`src/server/ha_state_machine.h:78,90`; loop
`src/server/ha_state_machine.cpp:487-504`, default 1000 ms) plus braft/brpc's own
library-managed threads.

## 4. On-disk Raft state

Both are **global per server, never per graph**.

### Bolt HA

- Path: `bolt_raft_logstore_path` if set, else `<db_dir>/raftlog`
  (`src/server/lgraph_server.cpp:328-331`). Registered with the argparser in
  Phase 3 (`src/core/global_config.cpp`), so it can now be set.
- Format: RocksDB with two column families (`src/bolt_raft/raft_driver.cpp:250-272`).
- meta CF keys: `hardState`, `applyIndex`, `confState`, `nodeInfos`
  (`src/bolt_raft/raft_log_store.cpp:30-33`).
- log keys: big-endian `uint64` index (`src/bolt_raft/raft_log_store.cpp:23-28`);
  a dummy index-0 entry is written on first init (`:41-56`).

### Legacy HA

Root `ha_log_dir`, default `<db_dir>/ha` (`src/server/lgraph_server.cpp:120-127`),
with three `local://` sub-URIs: `/log`, `/raft_meta`, `/snapshot`
(`src/server/ha_state_machine.cpp:56-59,96-99`).

## 5. Snapshots in Bolt HA

Bolt HA's snapshot story was hardened during Phase 3. The storage side now keeps
the etcd-raft storage contract:

- `RaftLogStorage` persists snapshot metadata (`snapIndex`/`snapTerm`) and
  exposes `SetSnapshot`, `GetSnapshotMeta` and `ApplySnapshot`
  (`src/bolt_raft/raft_log_store.{h,cpp}`). `ApplySnapshot` re-anchors the
  stable log at the incoming snapshot's index, rejecting out-of-date snapshots
  (`raft_log_store.cpp`), matching `MemoryStorage::ApplySnapshot`
  (`deps/etcd-raft-cpp/storage.h:205-223`).
- `RaftDriver::CheckReady` no longer asserts snapshots never appear; a snapshot
  in `ready.snapshot_` is persisted to storage instead of killing the process
  (`src/bolt_raft/raft_driver.cpp:488-494,519-523`).
- `RaftLogStorage::Snapshot()` still returns `ErrSnapshotTemporarilyUnavailable`
  until a snapshot has actually been created, which the raft core handles
  gracefully (`deps/etcd-raft-cpp/raft.h:796-804`).

**Safe log GC (the important fix).** Log compaction is the control that used to
strand slow followers. `RaftDriver::CheckAndCompactLog` now only compact to an
index that **every peer has acknowledged**: on the leader it reads
`rn_->raft_->trk_` (the ProgressTracker match index per peer) and refuses to
drop any entry beyond the slowest peer's `match_`
(`src/bolt_raft/raft_driver.cpp:450-493`). Consequences:

- A follower that is merely behind never gets past the compacted prefix; the
  leader holds off GC until the follower confirms the entries.
- A peer that has been offline for a long time does not block GC forever — the
  compaction ceiling is `min(applied - keep_logs, min_match)`, so once a peer
  reconnects and confirms recent entries, GC resumes up to the new floor.
- `keep_logs` defaults to 1,000,000 and `gc_interval` to 10 minutes
  (`src/core/global_config.h:96-99`).

Remaining limitation: a genuinely brand-new peer added to a group whose log is
already compacted still cannot bootstrap from a *data-carrying* snapshot,
because Bolt HA replicates Cypher text and the application state (the graph
DBs) is not serialized into `raftpb::Snapshot.data`. That remains a future
extension; safe GC prevents the common slow-follower case from ever needing it.

Legacy braft HA does have real snapshots, scheduled at
`ha_snapshot_interval_s` (default 7 days,
`src/core/global_config.h:52`, wired at `src/server/ha_state_machine.cpp:95`;
save path `src/server/ha_state_machine.cpp:275-294`, load `:296-323`). Note that
`LGRAPH_SHARE_DIR` changes snapshot semantics materially — under that flag only a
dummy snapshot carrying the log index is written
(`src/server/ha_state_machine.cpp:276-285`).

## 6. Graph attach/detach from Raft

**There is no per-graph attach or detach in either subsystem.** No code in
`src/bolt_raft/` references graph names or `CreateGraph`/`DeleteGraph`. The only
`ConfChange`s are cluster-membership operations exposed as Cypher procedures:

| Procedure | Location |
|---|---|
| `db.bolt.addRaftNode` | `src/cypher/procedure/procedure.cpp:3445-3477` |
| `db.bolt.addRaftLearnerNode` | `src/cypher/procedure/procedure.cpp:3479-3511` |
| `db.bolt.removeRaftNode` | `src/cypher/procedure/procedure.cpp:3513-3538` |

Driver side: `RaftDriver::ProposeConfChange`
(`src/bolt_raft/raft_driver.cpp:403-411`), applied at `:570-645` (adds/removes
`NodeClient`, persists `ConfState` + `NodeInfos` + apply index at `:626-630`).
braft membership uses `ha_conf` / `braft::cli::add_peer` / `remove_peer`
(`src/server/ha_state_machine.cpp:121-139,635-665`).

Graph creation affects Raft in **no** way beyond being an ordinary replicated
write. Raft metadata does not grow with graph count.

## 7. Recovery on restart

### Server startup order

```
LGraphServer::Start                              src/server/lgraph_server.cpp:116-386
  ├─ new StateMachine / HaStateMachine                     :240-246
  ├─ brpc + braft service registration                     :257-311
  ├─ state_machine_->Start()                               :313
  │    └─ new Galaxy(...) → ReloadFromDisk → open ALL graphs   src/server/state_machine.cpp:41-46
  │    └─ if enable_backup_log and snapshot_dir empty: take initial snapshot  :47-56
  ├─ BoltServer start                                      :321-326
  └─ BoltRaftServer start (reads persisted apply index)     :327-362
```

`HaStateMachine::Start` bootstraps or joins braft **after**
`StateMachine::Start` has already opened every graph
(`src/server/ha_state_machine.cpp:20-141`).

### Graph rediscovery

`GraphManager::ReloadFromDisk` iterates the graph config table and opens every
graph (`src/db/graph_manager.cpp:233-296`). Graphs present in memory but absent
from the table are erased and their directories removed (`:285-295`). If the
table is empty, `default` is created (`:242-255`). See
[01](01-graph-lifecycle.md).

### WAL / redo replay

TuGraph has no SQL-style redo log for Raft. Durability is LMDB (opened with
`MDB_NOSYNC`, so never fsynced) plus an optional per-store WAL:

- `durable=true` → one `Wal` per store (`src/core/lmdb_store.cpp:72-77`).
- `Wal::ReplayLogs()` replays on open (`src/core/wal.cpp:412-432`, impl `:459`).
- Each store publishes its last op id to the process-global
  `last_op_id_` atomic (`src/core/lmdb_store.cpp:66-71`;
  `src/core/lmdb_store.h:181-194`).

### Bolt HA catch-up

`BoltRaftServer::Start` reads the durable applied index from the Galaxy metadb
(`sm_->GetGalaxy()->GetBoltRaftApplyIndex()`,
`src/server/bolt_raft_server.cpp:52-53`). `RaftDriver::Run` then:

1. opens the RocksDB log store and calls `storage_->Init()` to read
   `hardState`/`confState` and the first/last log index
   (`src/bolt_raft/raft_log_store.cpp:35-81`),
2. computes `applied = max(galaxy_apply_index, storage_->GetApplyIndex())` and
   seeds the raft log with it (`src/bolt_raft/raft_driver.cpp:273,288`;
   `deps/etcd-raft-cpp/raft.h:629-630`) — this is what prevents re-applying
   already-applied entries on restart,
3. bootstraps only if the store was empty (`:303-314`).

The durable applied checkpoint for normal traffic lives in the **Galaxy metadb**
key `bolt_raft_apply_index` (`src/db/galaxy.cpp:788-803`), not in the RocksDB
`applyIndex`, which is only written on `ConfChange`
(`src/bolt_raft/raft_driver.cpp:629`). This split is easy to get wrong when
reasoning about recovery.

### Legacy HA catch-up

Bootstrap sets a synthetic starting log index
(`Galaxy::GetRaftLogIndex() + 1024`, `src/server/ha_state_machine.cpp:35,48-50`;
`Galaxy::BootstrapRaftLogIndex` `src/db/galaxy.cpp:805-811`). braft replays its
own `/log`; if it needs a snapshot it calls `on_snapshot_load` →
`Galaxy::LoadSnapshot` (`src/server/ha_state_machine.cpp:296-323`). Committed
entries replay through `on_apply` with an idempotency check against
`Galaxy::GetRaftLogIndex()` (`= LMDBKvStore::GetLastOpIdOfAllStores()`,
`src/db/galaxy.cpp:578`); old entries are skipped with "Skipping old request"
(`src/server/ha_state_machine.cpp:360-376`).

## 8. Scaling characteristics and risks

**Good news:** Raft cost is constant in graph count. One group, one node, fixed
threads, fixed timers, one log store.

**Real risks:**

1. **Bolt HA replication coverage is partial.** It only replicates the Bolt `Run`
   path (`src/server/bolt_handler.cpp:282-299`). Writes issued over REST, the RPC
   API, or admin operations are **not** replicated by Bolt HA. Mixing surfaces
   silently breaks replica consistency.
2. **Bolt HA cannot recover a lagging follower** (snapshots disabled, §5).
3. **Both HA modes are single-group**, so replication throughput does not scale
   with graphs — and a global Raft order is a global serialization point for all
   writes across all graphs.
4. **Legacy braft HA combined with many graphs is dangerous**: `on_apply`
   deserializes and applies whole requests, and `Galaxy::ReloadFromDisk` /
   `Galaxy::LoadSnapshot` are O(N graphs) and stop the world.
5. **Raft apply index and business state are in different stores**, with the
   authoritative applied checkpoint in the metadb. Any later metadata
   redesign must preserve this coupling.

For a horizontally scalable multi-graph platform, the single global Raft group is
the central architectural obstacle. Per-graph or sharded consensus groups — and
a decision about which front-end paths are replicated — will be required in a
later phase. See [07-scalability-risks.md](07-scalability-risks.md).
