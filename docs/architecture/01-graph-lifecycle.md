# 01 — Graph Lifecycle

This document traces how a graph is created, opened, used, closed and deleted,
and quantifies what one graph costs in threads, file descriptors, memory and
virtual address space. It is the core reference for graph-count scaling.

## 1. The three classes

### `lgraph::Galaxy` — the database instance

`src/db/galaxy.h:46`, implemented in `src/db/galaxy.cpp`.

`Galaxy` is the whole database. It owns:

| Member | Type | Location |
|---|---|---|
| `store_` | `std::unique_ptr<KvStore>` | `src/db/galaxy.h:69` — the `<db_dir>/.meta` LMDB env |
| `acl_` | `std::unique_ptr<AclManager>` | `src/db/galaxy.h:70` — server-wide users/roles |
| `graphs_` | `std::unique_ptr<GraphManager>` | `src/db/galaxy.h:72` |
| `token_manager_` | `TokenManager` | `src/db/galaxy.h:74` |
| `db_info_table_`, `ip_whitelist_table_`, `ip_whitelist_` | meta/whitelist | `src/db/galaxy.h:75-77` |
| `global_config_` | `std::shared_ptr<GlobalConfig>` | `src/db/galaxy.h:64-68` |
| `reload_lock_` | `KillableRWLock` | `src/db/galaxy.h:62` — the **global stop-the-world lock** |
| `acl_lock_`, `graphs_lock_`, `ip_whitelist_rw_lock_` | locks | `src/db/galaxy.h:71,73,78` |

There is a second, unrelated `lgraph_api::Galaxy` (`include/lgraph/lgraph_galaxy.h:31`,
implementation `src/lgraph_api/lgraph_galaxy.cpp:32-41`) which is a thin
embedder-facing wrapper holding an `lgraph::Galaxy*`. Do not confuse them.

Construction: `src/db/galaxy.cpp:44-69`. It validates `<dir>`, creates
`<dir>/.meta`, opens the meta LMDB env with a **hard-coded 1 GiB map**
(`src/db/galaxy.cpp:554`), then calls `ReloadFromDisk()`.

Destruction calls `GraphManager::CloseAllGraphs()`, which blocks until every
graph's reference count drains (`src/db/graph_manager.cpp:58-76`).

### `lgraph::GraphManager` — the graph registry

`src/db/graph_manager.h:36`, implemented in `src/db/graph_manager.cpp`.

```cpp
using GcDb = GCRefCountedPtr<LightningGraph>;        // src/db/graph_manager.h:62
std::unordered_map<std::string, GcDb> graphs_;       // src/db/graph_manager.h:65
std::shared_ptr<KvTable> table_;                     // _graph_config_table_ in .meta
std::string parent_dir_;
Config config_;
```

**There is no lock inside `GraphManager`.** All concurrency is provided
externally by `Galaxy::graphs_lock_`. Readers take a read lock; mutations take a
write lock. This is a single global lock covering every graph.

Graphs are looked up by name in an `unordered_map`, so a single open is O(1)
(`GraphManager::GetGraphRef`, `src/db/graph_manager.cpp:219-226`). Listing is
O(N) (`src/db/graph_manager.cpp:213-217`).

### `lgraph::LightningGraph` — one graph

`src/core/lightning_graph.h:42`, implemented in `src/core/lightning_graph.cpp`.

Owns, per graph (`src/core/lightning_graph.h:48-67`):

| Member | What it is |
|---|---|
| `config_` | `DBConfig` — name, dir, db_size, durable, plugin options |
| `store_` | **one LMDB env** dedicated to this graph |
| `meta_table_`, `blob_table_` | `_meta_` (secret, next vid, per-label counts), `_blob_` |
| `graph_` | `graph::Graph` over the `_graph_` table |
| `index_manager_` | index catalog and index tables |
| `plugin_manager_` | C++/Python plugin catalogs |
| `blob_manager_` | blob storage helpers |
| `fulltext_index_` | Lucene index (only if built with `ENABLE_FULLTEXT_INDEX`, default OFF) |
| `schema_` | `GCRefCountedPtr<SchemaInfo>` — copy-on-write schema snapshot |
| `meta_lock_` | `KillableRWLock` — the per-graph DDL lock |
| `db_secret` | guards against a graph dir being swapped in |

`DISABLE_COPY` / `DISABLE_MOVE` — a `LightningGraph` is never copied.

## 2. On-disk layout

Graph directories are named `Base16(8 random bytes) + Base16(timestamp)`
(`GraphManager::GenNewGraphSubDir`, `src/db/graph_manager.cpp:26-42`;
`GRAPH_SUBDIR_NAME_LEN = 8` at `src/core/defs.h:157`).

```
<db_dir>/
├── .meta/                          LMDB env: data.mdb + lock.mdb
│   └── tables: _meta_, _graph_config_table_, _ip_whitelist_,
│               _user_table_, _role_table_
├── <16-hex-subdir>/                LMDB env for graph "default"
│   ├── data.mdb                    graph data (sparse; only pages in use)
│   ├── lock.mdb                    LMDB reader/writer tables
│   ├── _cpp_plugin_/               created when load_plugins
│   ├── _python_plugin_/            created when python plugins enabled
│   ├── _fulltext_index_/           only with ENABLE_FULLTEXT_INDEX
│   └── wal.log.<id>, dbi.log       only when durable=true
└── <16-hex-subdir>/                graph #2 ... one directory per graph
```

Table names inside a graph's `data.mdb` (`src/core/defs.h:87-110,133-134`):
`_meta_`, `_blob_`, `_graph_`, `_v_schema_`, `_e_schema_`, `_v_index_`,
`_cpp_plugin_`, `_python_plugin_`, plus `_vertex_property_<label>` /
`_edge_property_<label>` for labels declared `DetachProperty`, plus one table per
index named `<label>_@lgraph@_<field>_@lgraph@_<type>`.

**Empty-graph disk cost** is small: LMDB writes two meta pages on env creation
(`Database::LMDB` `NUM_METAS = 2`, page = OS page size capped at 32 KiB), then
`LightningGraph::Open` runs one write transaction that creates the six base
tables (`src/core/lightning_graph.cpp:3030-3071`). The measured value is in the
baseline results; the `db_size` (default 4 TiB) is an mmap cap, **not**
preallocated disk, because LMDB uses sparse files.

## 3. Graph creation, end to end

The name→directory mapping is stored as a serialized `DBConfig` row in the
meta-store table `_graph_config_table_` (`StoreConfig`,
`src/db/graph_manager.cpp:50-56`).

```
REST   POST /db {name, config:{max_size_GB}}
  └─ RestServer::HandlePostGraph                     src/restful/server/rest_server.cpp:2506-2532
     └─ ApplyToStateMachine                          src/restful/server/rest_server.cpp:70-76
        └─ StateMachine::HandleRequest                src/server/state_machine.cpp:150-193
           └─ ApplyRequestDirectly  (kGraphRequest)  src/server/state_machine.cpp:281-286
              └─ ApplyGraphRequest                    src/server/state_machine.cpp:564-591
                 └─ Galaxy::CreateGraph               src/db/galaxy.cpp:184-207
                    ├─ new AclManager(*acl_)          src/db/galaxy.cpp:192
                    ├─ new GraphManager(*graphs_)     src/db/galaxy.cpp:193
                    ├─ GraphManager::CreateGraph      src/db/graph_manager.cpp:92-116
                    │  ├─ CheckValidGraphNum(N)       src/db/graph_manager.cpp:99
                    │  ├─ db_size default 4 TiB       src/db/graph_manager.cpp:103-104
                    │  ├─ GenNewGraphSubDir()         src/db/graph_manager.cpp:106
                    │  ├─ StoreConfig(txn, name, …)
                    │  └─ new LightningGraph(config)  src/db/graph_manager.cpp:112
                    │     └─ Open()                   src/core/lightning_graph.cpp:3026-3097
                    │        └─ new LMDBKvStore(...)  src/core/lmdb_store.cpp:45-78,96-105
                    ├─ acl_new->AddGraph(txn, …)      src/db/galaxy.cpp:199
                    ├─ txn.Commit()
                    └─ swap acl_ / graphs_            src/db/galaxy.cpp:203-204
```

Other entry points converge on the same core:

| Surface | Entry point |
|---|---|
| Bolt | **No graph create/delete.** Bolt only runs queries; graph is a Cypher string. |
| Cypher | `CALL dbms.graph.createGraph(name, desc, max_size_GB)` → `BuiltinProcedure::DbmsGraphCreateGraph` `src/cypher/procedure/procedure.cpp:2039-2064` |
| Embedded C++ | `lgraph_api::Galaxy::CreateGraph` `src/lgraph_api/lgraph_galaxy.cpp:290-297` |
| C API | `lgraph_api_galaxy_create_graph` `src/lgraph_api/c.cpp:2533-2541` |
| REST | `POST /db`, above |

### The O(N) cost inside `CreateGraph`

`src/db/galaxy.cpp:192-193`:

```cpp
std::unique_ptr<AclManager> acl_new(new AclManager(*acl_));
std::unique_ptr<GraphManager> gm_new(new GraphManager(*graphs_));
```

Both are **full copy-constructions performed under the `graphs_lock_` write
lock**, for every create/delete/modify. Copying `GraphManager` copies its entire
`unordered_map<std::string, GcDb>`: N string keys and N
`GCRefCountedPtr` copies. A `GCRefCountedPtr` copy is cheap (an atomic
`IncManagerRef`, `src/core/managed_object.h:205-209`) but it is still O(N) work
under a global exclusive lock. Copying `AclManager` is worse: it copies every
user and role, each holding a `graph → AccessLevel` map, so it is
O(users × graphs) per lifecycle operation (see [03](03-metadata-and-acl.md)).

Consequence: **creating N graphs one at a time is O(N²)**, and every graph
create/delete briefly blocks all graph opens and all listing. This is the single
most important graph-count scaling property in the codebase.

### The 4 TiB default

`GraphManager::CreateGraph` fills in the default when the caller passes
`db_size == 0` (`src/db/graph_manager.cpp:103-104`), and the default is
`DEFAULT_GRAPH_SIZE = 1 << 42` = 4 TiB on non-Windows
(`src/core/defs.h:151-156`). LMDB then `mmap`s that entire range with
`PROT_READ, MAP_SHARED`.

REST callers cannot hit this by accident: `JsonToType<DBConfig>` **requires**
`max_size_GB` and rejects non-positive values
(`src/restful/server/json_convert.h:536-548`). Cypher and the embedded API also
pass an explicit size. The path that silently gets 4 TiB is code that constructs
a `DBConfig` and leaves `db_size` at zero — including the auto-created `default`
graph at first boot (`src/db/graph_manager.cpp:242-255`).

For any 1000+ graph experiment you must set a small `max_size_GB` (the Phase 0
harness uses 1 GiB), otherwise the process exhausts virtual address space long
before it exhausts anything else.

## 4. Graph open

`LightningGraph::Open()` — `src/core/lightning_graph.cpp:3026-3097`:

1. `Close()` first, so `Open` is also `Reopen`.
2. `new LMDBKvStore(config_.dir, config_.db_size, config_.durable, create_if_not_exist)`
   (`:3028-3029`).
3. Open/create the base tables: `_meta_`, `_v_schema_`, `_e_schema_` (including
   one detached-property table per `DetachProperty` label), `_graph_`, `_v_index_`,
   `_blob_` (`:3032-3071`).
4. Commit that transaction.
5. If `config_.load_plugins`: construct `PluginManager` (`:3073-3078`).
6. If `config_.ft_index_options.enable_fulltext_index`: construct `FullTextIndex`
   (`:3079-3096`).

`WarmUp()` (`src/core/lightning_graph.cpp:3004-3006`) is only invoked on demand,
never during open.

### Per-graph background resources

| Resource | When | Where |
|---|---|---|
| **1 thread**: LMDB validator, polls every 100 ms | **always** | started `src/core/lmdb_store.cpp:105`; loop `:231-237` |
| 1 thread: WAL flusher | only `durable=true` | `src/core/wal.cpp:431` |
| 1 recurring task on the *global* scheduler | Python plugins enabled + loaded | `src/plugin/python_plugin.cpp:43-68` |
| ~2–3 Java threads | `ENABLE_FULLTEXT_INDEX` (default OFF) | `src/lucene/src/main/java/Lucene.java:45-63` |
| 1 recurring 1000 ms task while a deleted graph's refs drain | only on delete with outstanding refs | `src/core/managed_object.h:256-272` |

The validator thread is the important one: it is **unconditional, per graph, and
per graph env — including the Galaxy meta store**. At N graphs the process has
N+1 such threads, all waking 10×/second even when completely idle.

### Per-graph memory

Beyond the LMDB env itself:

- `GcDb` = `GCRefCountedPtr<LightningGraph>` allocates a
  `RefCountedObj<LightningGraph>` whose `references_` vector has
  `LGRAPH_MAX_THREADS = 480` entries (`src/core/thread_id.h:21`) of
  `PadForCacheLine<uint64_t>` = 64 bytes each
  (`include/fma-common/type_traits.h:178-186`). That is **~30 KiB per graph**.
- `LightningGraph::schema_` is another `GCRefCountedPtr<SchemaInfo>` ⇒ another
  **~30 KiB per graph**.

So roughly **60 KiB of refcount arrays per graph before any real data**, and the
arrays scale with `LGRAPH_MAX_THREADS`, not with graph count. Note also that
`PadForCacheLine` adds padding but no `alignas`, so the entries are 64 bytes in
size but not actually cache-line aligned.

## 5. Graph deletion, end to end

```
REST   DELETE /db/{name}
  └─ RestServer::HandleDeleteGraph        src/restful/server/rest_server.cpp:2675-2696
     └─ … ApplyGraphRequest → Galaxy::DeleteGraph   src/db/galaxy.cpp:210-238
```

Inside `DeleteGraph` (`src/db/galaxy.cpp:210-238`):

1. Copy-on-write `AclManager` and `GraphManager` (`:217-218`), as with create.
2. Hold a `ScopedRef` on the graph so it cannot vanish mid-operation (`:219`).
3. Remove the ACL entry and the map entry, writing through to the meta store.
4. Commit, then swap in the new managers (`:228-229`).
5. Schedule physical destruction for when the last reference drops (`:231-236`):

```cpp
db.Assign(nullptr, [](LightningGraph* db) {
    std::string dir = db->GetConfig().dir;
    db->Close();
    fma_common::FileSystem::GetFileSystem(dir).RemoveDir(dir);
});
```

`GraphManager::DelGraph` only erases the map entry and the meta-store key
(`src/db/graph_manager.cpp:164-174`) — it returns the `GcDb` so the caller
controls destruction. If any transaction still holds a reference, the directory
lingers and a recurring GC task is scheduled on the global `TimedTaskScheduler`
(`src/core/managed_object.h:256-272`).

If a graph dir exists on disk but has no config row, `ReloadFromDisk` removes it
at the next startup (`src/db/graph_manager.cpp:285-295`).

## 6. Startup and shutdown

### Startup

```
LGraphServer::Start                            src/server/lgraph_server.cpp:116-386
  ├─ derive ha_log_dir / backup_log_dir / snapshot_dir    :120-137
  ├─ LoggerManager::Init                                  :141-156
  ├─ AuditLogger::Init                                    :206-212
  ├─ new StateMachine / HaStateMachine                    :240-246
  ├─ crossplat::threadpool::initialize_with_threads       :250-256
  ├─ brpc::Server + services (if enable_rpc)              :263-311
  ├─ state_machine_->Start()          ← opens ALL graphs  :313
  │    └─ new Galaxy(conf, true, global_config_)          src/server/state_machine.cpp:41-46
  │       └─ Galaxy::ReloadFromDisk                       src/db/galaxy.cpp:540-576
  │          └─ GraphManager::ReloadFromDisk              src/db/graph_manager.cpp:233-296
  │             └─ for each row: new LightningGraph(conf) :257-284   ← SEQUENTIAL
  ├─ RestServer (constructor starts listening)            :318-320
  ├─ BoltServer::Instance().Start(...)                    :321-326
  ├─ BoltRaftServer::Instance().Start(...) (optional)     :327-362
  └─ LOG_INFO() << "Server started."                      :379
```

`GraphManager::ReloadFromDisk` (`src/db/graph_manager.cpp:233-296`) is a plain
single-threaded loop over the config table with no parallelism. Server start is
therefore **sum over all graphs** of (LMDB env open + table opens + plugin
manager + optional fulltext), and the process does not accept requests until it
finishes. This is the primary startup-time scaling risk.

Notable extras at startup:

- If the config table is empty, a `default` graph is created
  (`src/db/graph_manager.cpp:242-255`) — this one uses the 4 TiB default.
- If `enable_backup_log` is on **and** `snapshot_dir` is empty, a full snapshot
  is taken at startup (`src/server/state_machine.cpp:47-56`).
- Vector indexes are rebuilt in memory by a full scan of their label's detached
  property table on **every** open (`src/core/index_manager.cpp:97-147`), so
  those graphs' open cost is O(rows), not O(1). Normal and composite indexes are
  **not** rebuilt (`src/core/index_manager.cpp:47-51,60-64`).

### Shutdown

```
LGraphServer::Stop                              src/server/lgraph_server.cpp:400-451
  ├─ TaskTracker::GetInstance().KillAllTasks()           :426
  ├─ rpc_server_->Stop() / Bolt / BoltRaft stops         :434-439
  └─ state_machine_->Stop()                              :441
       └─ reset Galaxy → Galaxy::~Galaxy                 src/db/galaxy.cpp:71-74
          └─ GraphManager::CloseAllGraphs                src/db/graph_manager.cpp:58-76
             └─ wait until every graph ref drains, then clear
```

`CloseAllGraphs` assigns `nullptr` to each entry and waits on a condition
variable until all N graphs have been destroyed. Each destruction closes the
LMDB env and joins the validator thread
(`src/core/lmdb_store.cpp:108-116`). Shutdown is therefore O(N), and it **blocks
if any graph still has an outstanding reference** (a long-running transaction or
a plugin).

## 7. What scales with graph count

| Cost | Scaling | Where |
|---|---|---|
| Process startup time | O(N), sequential | `src/db/graph_manager.cpp:257-284` |
| Process shutdown time | O(N), with a blocking wait | `src/db/graph_manager.cpp:58-76` |
| Threads | 1 per graph + 1 for meta | `src/core/lmdb_store.cpp:105` |
| File descriptors | ~3 per graph measured (2 LMDB files `data.mdb`/`lock.mdb`, plus one more not identified in Phase 0) | LMDB env per graph; `benchmark/scaling/results/BASELINE.md` |
| Virtual address space | `db_size` per graph (4 TiB default) | `src/db/defs` `src/core/defs.h:154` |
| Refcount arrays | ~60 KiB per graph | `src/core/managed_object.h:55-60` |
| Graph create / delete / modify latency | O(N) under a global write lock | `src/db/galaxy.cpp:192-193,217-218` |
| ACL memory after create/delete churn | O(users × graphs) copies | `src/db/acl.h:139-203` |
| List graphs latency | O(N) | `src/db/graph_manager.cpp:213-217` |
| Snapshot / backup / load | O(N), blocking | [05](05-backup-restore-snapshot.md) |
| Per-graph idle CPU | 10 validator wakeups/s | `src/core/lmdb_store.cpp:237` |

Components that later phases will most likely have to change: `GraphManager`
(ownership, locking, lazy open), `Galaxy::CreateGraph`/`DeleteGraph`/`ModGraph`
(copy-on-write of whole managers), `LMDBKvStore` (per-graph validator thread),
`GraphManager::ReloadFromDisk` (sequential eager open), and the `RefCountedObj`
thread-slot array. `MAX_NUM_GRAPHS` has already been replaced by the
configurable `max_graphs` setting. See
[07-scalability-risks.md](07-scalability-risks.md).
