# 05 — Snapshot, Backup, Binlog and Restore

There are **three distinct things called "snapshot"** plus a separate
request-level binlog. Confusing them is easy and consequential, so this document
separates them first, then covers backup and restore.

## 0. The four mechanisms

| # | Mechanism | Scope | Blocking? | Where |
|---|---|---|---|---|
| A | Bolt-Raft Raft snapshot | Raft log | recorded (index/term/conf-state); catch-up by log replay under safe GC | `src/bolt_raft/raft_log_store.{h,cpp}`, `src/bolt_raft/raft_driver.cpp` (safe compaction) |
| B | braft Raft snapshot | whole server | braft-scheduled | `src/server/ha_state_machine.cpp:275-323` |
| C | Business "backup snapshot" (`dbms.takeSnapshot`) | whole server, all graphs | holds Galaxy `reload_lock_` read | `src/server/state_machine.cpp:202-234`, `src/db/galaxy.cpp:505-531` |
| D | Incremental binlog | whole server, request-level | online (append per write) | `src/core/backup_log.h`, `src/server/state_machine.cpp:150-163` |

Mechanism A is covered in [04](04-ha-raft-replication.md); it is a Raft log
snapshot, not a data snapshot. It carries metadata (index/term/conf-state);
`Snapshot()` returns the stored snapshot when one exists and
`ErrSnapshotTemporarilyUnavailable` only when none has been recorded yet.

## 1. Business snapshot (`dbms.takeSnapshot`)

### Trigger points

1. **Server start**, but only if `enable_backup_log` is on **and** `snapshot_dir`
   contains no subdirectories (`src/server/state_machine.cpp:47-56`).
2. **`CALL dbms.takeSnapshot()`**
   (`src/cypher/procedure/procedure.cpp:2349-2365`).

### Implementation

`StateMachine::TakeSnapshot()` (`src/server/state_machine.cpp:202-234`):

- builds a path `snapshot_dir/<timestamp>` with `:` → `.` and space → `_`
  substitutions,
- calls `TakeSnapshot(path, true)`, which calls `galaxy_->SaveSnapshot(path)` and
  then `backup_log_->TruncateLogs()`,
- deletes older snapshot directories after success.

`Galaxy::SaveSnapshot` (`src/db/galaxy.cpp:505-531`):

```cpp
// TODO: for now, we require TLSRWLock write lock to be held before saving snapshot
// However, we should be able to leverage MVCC here and thus avoid locking the whole galaxy.
_HoldReadLock(reload_lock_);          // <-- read, not write, despite the TODO
...
store_->Backup(meta_dir);             // metadb
auto files = graphs_->Backup(dir);    // ALL graphs, one by one
```

`GraphManager::Backup` (`src/db/graph_manager.cpp:298-316`) iterates every graph
and does a full LMDB copy per graph:

```cpp
for (auto& kv : graphs_) {
    ScopedRef<LightningGraph> g = kv.second.GetScopedRef();
    ...
    g->Backup(graph_dir);
}
```

Per-graph copy primitive: `LMDBKvStore::Backup` / `Snapshot`
(`src/core/lmdb_store.cpp:197-208`) using `mdb_env_copy2` / `mdb_env_copy_txn`.

### Scaling properties — the important part

- **O(N graphs)** and strictly sequential.
- Holds the Galaxy `reload_lock_` for the entire duration. It is a **read** lock,
  so ordinary requests (which also take read locks) proceed — but any
  `ReloadFromDisk`, config update, or full backup must wait.
- The comment at `src/db/galaxy.cpp:506-508` acknowledges the lock is wrong and
  that MVCC should be used instead. This is a known, documented limitation.
- The snapshot is the **whole server**: there is no per-graph snapshot.

## 2. Full backup (`lgraph_backup`)

CLI: `toolkits/lgraph_backup.cpp:23-92`, args `--src`, `--dst`, `--compact`
(default true).

```cpp
lgraph::Galaxy src_galaxy(src, false);          // opens the DB directly (offline)
_HoldWriteLock(src_galaxy.GetReloadLock());     // exclusive
src_galaxy.Backup(dst, compact);
```

`Galaxy::Backup` (`src/db/galaxy.cpp:590-604`):

```cpp
_HoldWriteLock(reload_lock_);                   // blocks ALL reads and writes
store_->Backup(meta_dir, compact);              // metadb
for (auto& kv : ListGraphs(DEFAULT_ADMIN_NAME)) // then every graph
    OpenGraph(...).Backup(graph_dir, compact);
```

- **Whole server, all graphs.** Not per-graph selectable.
- **Blocking**: takes the `reload_lock_` **write** lock, so the entire server
  stops serving for the duration of an O(N) copy.
- `compact` maps to `MDB_CP_COMPACT` (`src/core/lmdb_store.cpp:197-200`).
- Output is a directory tree mirroring `db_dir`: `.meta/data.mdb` plus one
  subdirectory per graph.
- There is **no REST endpoint and no RPC method** for full backup; it is a
  standalone offline tool. (REST/RPC have restore but not backup — see §4.)
- Minor stale detail: `toolkits/lgraph_backup.cpp:51` probes `.meta/data.lgr`;
  the actual file is `.meta/data.mdb`.

### Why it is "offline"

The tool opens its own embedded `Galaxy` on the source directory rather than
talking to a running server. Running it against a directory that a live server
has open is therefore a misuse; LMDB's lock file will typically prevent it.

## 3. Incremental binlog

Enabled by `enable_backup_log`; directory `backup_log_dir`, default
`<db_dir>/binlog` (`src/server/lgraph_server.cpp:128-130`).

- Every write request is appended **before** execution
  (`src/server/state_machine.cpp:150-163`):
  ```cpp
  if (is_write && backup_log_) backup_log_->Write(req);
  ```
- `BackupLog` (`src/core/backup_log.h:23-88`) writes rotating files
  `binlog_<N>` via `fma_common::RotatingFiles`
  (`:29-36`), max file size `max_backup_log_file_size` (default 1 GiB,
  `src/core/global_config.h:75`).
- Each record is a length-prefixed `BackupLogEntry{index, time, req}`
  (`:67-77`).
- `TruncateLogs()` is called whenever a snapshot is taken (`:83-87`), so the
  binlog covers "since the last snapshot".
- Listing is exposed as `CALL dbms.listBackupFiles()`
  (`src/cypher/procedure/procedure.cpp:2330-2347`).

Because it is request-level and only on the state-machine path, it inherits the
same coverage caveat as Bolt HA: **Bolt requests bypass the state machine and are
not logged here** (see [06](06-server-and-request-paths.md) §4).

## 4. Restore

### Full restore — filesystem copy, no dedicated tool

There is **no restore binary**. The documented procedure (see
`test/test_backup_restore.cpp:76-100`) is:

1. copy a backup tree, or a `snapshot_dir/<timestamp>` tree, into a fresh
   `db_dir`;
2. start `lgraph_server` on it.

`Galaxy::LoadSnapshot(dir)` (`src/db/galaxy.cpp:481-496`) is the in-process
variant, used only by braft's `on_snapshot_load`:

```cpp
_HoldWriteLock(reload_lock_);
auto confs = graphs_->ListGraphs();
store_.reset(nullptr);
graphs_->CloseAllGraphs();
graphs_.reset();
for (auto& conf : confs) fs.RemoveDir(conf.second.dir);   // delete every graph dir
fs.RemoveDir(metaDir);
fs.CopyToLocal(dir, config_.dir);
ReloadFromDisk(false);                                    // reopen everything
```

This is stop-the-world, O(N graphs), and deletes then recreates the entire
database directory tree.

Per-graph replace primitives exist for Raft use:
`LMDBKvStore::ReopenFromSnapshot` (`src/core/lmdb_store.cpp:80-92`) and
`LightningGraph::LoadSnapshot` (`src/core/lightning_graph.cpp:2992-3000`).

### Point-in-time restore from binlog — online

Proto: `RestoreRequest{ repeated BackupLogEntry logs }` /
`RestoreResponse{ last_success_idx }` (`src/protobuf/ha.proto:763-776`).

- Server apply: `StateMachine::ApplyRestoreRequest`
  (`src/server/state_machine.cpp:536-556`) — admin-only, re-applies each logged
  request through `ApplyRequestDirectly` and reports the last successful index.
- RPC client: `RpcClient::RpcSingleClient::Restore`
  (`src/client/cpp/rpc/lgraph_rpc_client.cpp:69-75`); aggregate
  `RpcClient::Restore` (`:953-962`).
- CLI: `toolkits/lgraph_binlog.cpp:68-248`, `-a restore`:
  - selects/sorts/filters binlog files by time and index
    (`:122-155`),
  - remote mode: batches up to 16 MiB per `Restore` call
    (`:167-188`),
  - local mode: opens an embedded `StateMachine`, `Start()`s it, then applies
    each entry via `ApplyRequestDirectly` (`:189-219`).
  - `-a print` inspects logs without applying (`:220-237`).

## 5. Backup/restore entry-point summary

| Operation | Surface | Per-graph? | Blocks server? |
|---|---|---|---|
| Snapshot | `CALL dbms.takeSnapshot()` | no | holds `reload_lock_` read |
| Snapshot (auto) | server start | no | during startup |
| Full backup | `lgraph_backup` CLI (offline) | no | n/a (offline) |
| Incremental log | always on if `enable_backup_log` | no | no (append only) |
| List backup files | `CALL dbms.listBackupFiles()` | no | no |
| Restore from binlog | `lgraph_binlog -a restore` (RPC or embedded) | no | applies writes |
| Full restore | manual directory copy + start server | no | restart |
| Raft snapshot | internal | no | n/a |

There are **no REST endpoints** for backup, restore or snapshot in this version.
`RestStrings::RESTORE` exists (`src/restful/server/json_convert.h:157`) but is
unused in `rest_server.cpp`.

## 6. Why this matters for graph count

Everything in this document is **whole-server and O(N graphs)**:

| Operation | Cost | Blocking behaviour |
|---|---|---|
| `Galaxy::SaveSnapshot` | O(N) sequential LMDB copies | holds `reload_lock_` read for the whole run |
| `GraphManager::Backup` | O(N) sequential LMDB copies | called by both snapshot and full backup |
| `Galaxy::Backup` | O(N) | holds `reload_lock_` **write** — full outage |
| `Galaxy::LoadSnapshot` | O(N) delete + copy + O(N) reopen | holds `reload_lock_` **write**; deletes all graph dirs first |
| Restore via binlog | O(entries), each a full request apply | online but serial |

At 1000+ graphs a snapshot or backup becomes a long, serial, all-or-nothing
operation with no per-graph granularity and no progress reporting. There is:

- **no online full backup** (the only full backup is offline and takes an
  exclusive lock if run in-process),
- **no per-graph backup/restore**,
- **no incremental data backup** — the binlog is request-level and only covers
  state-machine-path writes,
- **no parallel copy** — `GraphManager::Backup` is a serial loop.

Later phases will need per-graph or sharded backup/restore, parallelism across
graphs, and MVCC-based online snapshots as the code's own TODO suggests. See
[07-scalability-risks.md](07-scalability-risks.md).
