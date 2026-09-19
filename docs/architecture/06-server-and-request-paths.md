# 06 — Server Startup, Threads and Request Paths

## 1. Entry point and startup order

`src/server/server_main.cpp:34` is `main()`. It parses `-c/--config` and
`-d/--mode run|start|stop|restart` (`:39`), loads the JSON config (`:48-56`),
builds a `GlobalConfig` (`:58`), parses CLI overrides (`:59-68`), then wraps
everything in `LGraphDaemon` (a `Service` with pid file `./lgraph.pid`) and
dispatches (`:86-95`). `run` calls `server_.Start()` then `WaitTillKilled()`.

`LGraphServer::Start` (`src/server/lgraph_server.cpp:116-386`) in order:

| Step | Line | What |
|---|---|---|
| 1 | `:118` | `AccessControlledDB::SetEnablePlugin(config_->enable_plugin)` (process-wide) |
| 2 | `:120-137` | derive `ha_log_dir`, `backup_log_dir`, `snapshot_dir`; clamp `ha_snapshot_interval_s ≤ 21d` |
| 3 | `:139` | `rpc_port = http_port + 1` if unset |
| 4 | `:141-156` | init lgraph logging |
| 5 | `:163-166` | `enable_ha` forces `enable_rpc = true` |
| 6 | `:193-203` | log rlimits |
| 7 | `:206-212` | init audit logger |
| 8 | `:214-238` | brpc/braft logging |
| 9 | `:240-246` | **construct `HaStateMachine` or `StateMachine`** |
| 10 | `:250-256` | if `thread_limit != 0`: `crossplat::threadpool::initialize_with_threads(...)` |
| 11 | `:257-311` | brpc server + `LGraphRPCService` + `HttpService` + optional braft service; `Start()` with up to 5 retries |
| 12 | `:313` | **`state_machine_->Start()` — opens Galaxy and ALL graphs** |
| 13 | `:314-316` | optional unlimited token |
| 14 | `:318-320` | construct `RestServer` (its constructor calls `Start()`) |
| 15 | `:321-326` | `BoltServer::Instance().Start(...)` |
| 16 | `:327-362` | if `bolt_raft_port > 0`: `BoltRaftServer::Instance().Start(...)` |
| 17 | `:365-367` | if `enable_rpc`: `http_service_->Start(config_.get())` |
| 18 | `:368-378` | optional admin password reset, then stop |
| 19 | `:379` | `LOG_INFO() << "Server started."` |

The readiness signal used by the test/benchmark harness is that
`"Server started."` line. It is emitted **after** all graphs are open, so
time-to-that-line is exactly the graph-count-sensitive startup metric.

Shutdown is `LGraphServer::Stop` (`:400-451`): kill tracked tasks (`:426`), stop
RPC/Bolt/BoltRaft (`:434-439`), then `state_machine_->Stop()` (`:441`) which
destroys `Galaxy` and therefore closes all graphs. Signal handlers are installed
by `WaitTillKilled` (`:388-398`, `SetupSignalHandler` `:71-94`) for
SIGINT/SIGTERM/SIGUSR1 and for crash signals.

## 2. Global vs per-graph state

### Process-global

| Thing | Location |
|---|---|
| `StateMachine` / `HaStateMachine` | `src/server/state_machine.h:85`, `src/server/ha_state_machine.h:19` |
| `cypher::Scheduler` (one per state machine) | `src/server/state_machine.h:149,166` |
| `Galaxy` | `src/server/state_machine.h:147` |
| `BoltServer` singleton | `src/server/bolt_server.h:27` |
| `BoltRaftServer` singleton | `src/server/bolt_raft_server.h:26-50` |
| `DBManagementClient` singleton | `src/server/db_management_client.h:46` |
| `TaskTracker` singleton | `src/core/task_tracker.h:291` |
| `AuditLogger` singleton | `src/core/audit_logger.h:198` |
| `AllocatorManager` singleton | `src/cypher/monitor/monitor_manager.cpp:17` |
| `RestServer`, brpc `Server`, `HttpService` | `src/server/lgraph_server.h:86-91` |
| `TimedTaskScheduler` singleton | `include/fma-common/timed_task.h:106-123` |

### Per-graph

`LightningGraph` only: its own LMDB env, `graph::Graph`, vertex/edge
`SchemaManager`s, `IndexManager`, `BlobManager`, `PluginManager`, optional
`FullTextIndex`, `meta_lock_` (`src/core/lightning_graph.h:48-67`). Held through
`GraphManager::GcDb` (`src/db/graph_manager.h:62,65`).

## 3. Thread pools and thread budget

| Pool / thread | Size | Knob | Where |
|---|---|---|---|
| REST/cpprest pool | `thread_limit` (0 ⇒ cpprest default) | `thread_limit` | `src/server/lgraph_server.cpp:250-256` |
| brpc RPC | brpc internal; `max_concurrency`/`num_threads` **commented out** | none | `src/server/lgraph_server.cpp:286-287` |
| HTTP overlay | `import_pool_(1)`, `algo_pool_(4)` hard-coded | none | `src/http/http_server.h:161-162`, `src/http/http_server.cpp:142` |
| Bolt listener | 1 thread | — | `src/server/bolt_server.cpp:31-48` |
| Bolt IO workers | `bolt_io_thread_num` (default 1) | `bolt_io_thread_num` | `src/bolt/io_service.h:32-53,96` |
| Bolt per-connection FSM | 1 detached `std::thread` **per authenticated connection** | — | `src/server/bolt_handler.cpp:380-381` |
| HA heartbeat | 1 | — | `src/server/ha_state_machine.h:78,90` |
| Bolt-Raft driver | 4 + listener + 1 io-worker | — | `src/bolt_raft/raft_driver.cpp:318-337` |
| **LMDB validator** | **1 per graph + 1 meta** | — | `src/core/lmdb_store.cpp:105` |
| WAL flusher | 1 per durable store | — | `src/core/wal.cpp:431` |
| Import V3 | per-job asio pool | `read_rocksdb_threads` | `src/import/import_v3.cpp:243-245,521-523,1128-1131` |

The Bolt model deserves emphasis: **one OS thread per authenticated Bolt
connection**, detached and long-lived (`src/server/bolt_handler.cpp:380-381`).
Combined with one thread per graph, a multi-graph server with many Bolt clients
reaches thread-count limits quickly. TuGraph caps Bolt connections through
`LGRAPH_MAX_THREADS = 480` thread-id slots (`src/core/thread_id.h:21`), which is
also the size of every `RefCountedObj` reference array.

## 4. Request paths

### REST → state machine

```
cpprest listener                     src/restful/server/rest_server.cpp:554-567
  └─ handle_get/post/put/del         :2968,3055,3191,2893
     ├─ GetUser (Bearer token)       :569-584
     └─ ApplyToStateMachine          :70-76
        └─ StateMachine::HandleRequest           src/server/state_machine.cpp:150-193
           ├─ IsWriteRequest                     :77  (may run DetermineReadOnly)
           └─ DoRequest                          :236
              └─ ApplyRequestDirectly            :242
                 ├─ _HoldReadLock(galaxy->GetReloadLock())   :251
                 └─ ApplyGraphQueryRequest       :951-1055
                    └─ Scheduler::Eval           :1011
                       └─ ExecutionPlan(V2)::Execute
                          └─ Galaxy::OpenGraph → GraphManager::GetGraphRef
                             └─ per-graph LMDB txn
```

### Bolt — bypasses the state machine

```
asio accept                        src/bolt/io_service.h:103-121
  └─ Connection chunk decode       src/bolt/connection.cpp:251-312
     └─ BoltHandler                src/server/bolt_handler.cpp:323-466
        ├─ Hello  → ValidateUser, spawn BoltFSM thread     :327-384
        └─ Run    → session queue                          :385-396
             └─ BoltFSM (detached thread)                  :165-321
                ├─ read graph from extra["db"]             :254-258
                ├─ build RTContext                         :262-263
                ├─ if BoltRaft started and write: ProposeRaftRequest, wait  :282-299
                └─ Scheduler::Eval                         :302
```

**Critical difference:** `BoltFSM` calls `Scheduler::Eval` directly and **does
not go through `StateMachine::HandleRequest` / `ApplyRequestDirectly`**. It
therefore does **not** take `galaxy_->reload_lock_`. Only `Galaxy::OpenGraph` is
taken, which acquires `acl_lock_` + `graphs_lock_` (`src/db/galaxy.cpp:380-388`).

Two consequences:

1. A concurrent `Galaxy::ReloadFromDisk` (triggered by a config update or
   snapshot load, which takes the write lock) can race with in-flight Bolt
   queries, because Bolt readers hold no `reload_lock_` at all.
2. Bolt requests are **not logged to the binlog** (`StateMachine::HandleRequest`
   does that at `src/server/state_machine.cpp:150-163`) and are **not replicated
   by legacy braft HA**. Bolt HA replicates them through its own path instead.

### brpc RPC

`LGraphRPCService.HandleRequest` → `RPCService::HandleRequest`
(`src/server/lgraph_server.h:61`) → `StateMachine::HandleRequest`. The
`LGraphHttpService` overlay (`src/http/http_server.cpp:216-249`) maps method names
to handlers (`InitFuncMap` `:151-213`).

### OpenCypher execution

`Scheduler::Eval` (`src/cypher/execution_plan/scheduler.cpp:46-57`) dispatches:

- `EvalCypher` (v1) `:69-151` — ANTLR `Lcypher` grammar
  (`src/cypher/grammar/Lcypher.g4`, generated parser under
  `src/cypher/parser/generated`), `CypherBaseVisitor`.
- `EvalCypher2` (v2, default `is_cypher_v2=true`) `:167-268` — ANTLR →
  `CypherBaseVisitorV2` → geax AST (`deps/geax-front-end`) → rewriters →
  `ExecutionPlanV2`.
- `EvalGql` `:270-323` — `geax::frontend::AntlrGqlParser` directly.

Per-query allocations: `cypher::RTContext` on the stack
(`src/cypher/execution_plan/runtime_context.h:62-92`), an
`ObjectArenaAllocator` for the v2 AST, plus `AccessControlledDB`,
`lgraph_api::Transaction`, `ResultInfo`, `Result` created in
`ExecutionPlan::Execute` (`src/cypher/execution_plan/execution_plan.cpp:1394-1424`)
and `ExecutionPlanV2::Execute`
(`src/cypher/execution_plan/execution_plan_v2.cpp:68-92`).

**Plan caches are `thread_local` and keyed only by the query string**
(`src/cypher/execution_plan/scheduler.cpp:74`, `:170`; capacity 256,
`src/cypher/execution_plan/lru_cache.h:95`). A plan built on one thread is not
shared with other threads, so N Bolt FSM threads each build their own plan for
the same query. GQL has no plan cache at all. Whether cached plans embed
graph/schema-specific state from `PreValidate(ctx, ...)` is worth verifying in a
later phase.

## 5. Global locks on the request path

| Lock | Scope | Frequency |
|---|---|---|
| `Galaxy::reload_lock_` (read) | process | every RPC/REST request (`state_machine.cpp:251`, `:87`); **not** Bolt |
| `Galaxy::acl_lock_` (read) | process | token validation (`galaxy.cpp:125`), `OpenGraph` (`:382`), `IsAdmin` (`:534`) — at least once per request |
| `Galaxy::graphs_lock_` (read) | process | `OpenGraph` (`galaxy.cpp:386`), `ListGraphsInternal` (`:264`) |
| `AllocatorManager` mutex | process | **every allocation** in Cypher/GQL execution (`src/cypher/monitor/memory_monitor_allocator.h:41,49`; `monitor_manager.h:48,56`) |
| `InterruptableTLSRWLock::WriteLock` | process | scans `FMA_MAX_THREADS` reader slots (`include/fma-common/rw_lock.h:142-153`) |
| `BoltConnection::PostResponse` backpressure | per connection | busy-wait while queue > 1024 (`src/bolt/connection.cpp:153-155`) |
| Raft apply | process | all HA writes, one global order |

The `AllocatorManager` mutex is easy to overlook: it is taken on **every
individual allocate/deallocate** during query execution, so it is a
process-wide serialization point proportional to allocation count, not to
request count.

## 6. Linear scans over all graphs

| Operation | Where | Trigger |
|---|---|---|
| `GraphManager::ListGraphs` | `src/db/graph_manager.cpp:213-217` | `GET /db`, `dbms.graph.listGraphs()` |
| `Galaxy::ListGraphsInternal` | `src/db/galaxy.cpp:263-266` | REST graph listing |
| `GraphManager::Backup` | `src/db/graph_manager.cpp:298-316` | snapshot / full backup |
| `Galaxy::Backup` | `src/db/galaxy.cpp:590-604` | offline backup |
| `GraphManager::ReloadFromDisk` | `src/db/graph_manager.cpp:257-284` | startup, config update, snapshot load |
| `GraphManager::CloseAllGraphs` | `src/db/graph_manager.cpp:58-76` | shutdown |
| `Galaxy::OpenUserGraphs` | `src/db/galaxy.cpp:390-402` | ACL-driven listing |
| `TaskTracker::ListRunningTasks` / `KillAllTasks` | `src/core/task_tracker.h:354-370` | monitoring, shutdown |

Normal single-graph lookup is **not** a scan: `Galaxy::OpenGraph` →
`GraphManager::GetGraphRef` → `unordered_map::find`
(`src/db/graph_manager.cpp:219-226`). ACL checks use in-memory hash maps
(`src/db/acl.cpp:186-200`), and `IsAdmin` is O(1) (`:549-553`).

## 7. Configuration knobs relevant to scaling

Struct: `src/core/global_config.h` (`BasicConfigs` `:30-104`,
`GlobalConfig : BasicConfigs` `:117-131`). Defaults are applied in
`GlobalConfig::InitConfig` (`src/core/global_config.cpp:174-233`) and registered
with the argparser (`:236-379`).

| Knob | Default | Where | Relevance |
|---|---|---|---|
| `thread_limit` | 0 (cpprest default) | `global_config.cpp:194,291-292` | REST worker pool |
| `bolt_io_thread_num` | 1 | `global_config.cpp:220,344-345` | Bolt IO |
| `bolt_raft_logstore_threads` | 4 | `global_config.cpp:231` | RocksDB log store |
| `durable` | false | `data_type.h:156`, `global_config.h:119` | adds 1 WAL thread + fsync per store |
| `optimistic_txn` | false | `global_config.h:120` | concurrent write txns |
| `enable_backup_log` | false | `global_config.h` | binlog + startup snapshot |
| `is_cypher_v2` | true | `global_config.h` | v1 vs v2 execution path |
| `max_backup_log_file_size` | 1 GiB | `global_config.h:75` | binlog rotation |
| `enable_ha` / `bolt_raft_port` | false / 0 | `global_config.h:48,87` | selects HA mode |
| `db_dir` | `./lgraph_db` (default) / `/var/lib/lgraph/data` (packaged config) | `global_config.h:31` | where `.meta` and graph dirs live |

Graph count is bounded by the configurable `max_graphs` setting
(`src/core/defs.h`, `CheckValidGraphNum`; 0 means no application-level limit),
which replaced the former hard-coded `MAX_NUM_GRAPHS = 4096`. Per-graph mmap is
per-graph `DBConfig::db_size`, settable only at graph creation time (or via
`dbms.graph.modGraph`). Two engine-wide knobs also affect the per-graph memory
cost: `lmdb_notls` and `lmdb_max_dbs`.

Sample configs: `release/local/etc/lgraph.json` (shipped),
`src/server/lgraph_standalone.json`, `lgraph_ha.json`, `lgraph_daemon.json`,
`demo/movie/lgraph.json`.

## 8. Client tooling

| Tool | Protocol | Default | Notes |
|---|---|---|---|
| `lgraph_cli` | Bolt | `127.0.0.1:7687`, user `admin`, pass `73@TuGraph`, graph `default` | `toolkits/lgraph_cli.cpp:193-211`; handshake `:241-247`; query loop `:292-328`; supports `--format table|csv|json` |
| `lgraph_cypher` | — | — | **removed from the build**; do not use |
| RPC C++ client | brpc | `rpc_port` | `src/client/cpp/rpc/lgraph_rpc_client.cpp` |
| REST client | HTTP | `http_port` | `src/client/cpp/restful/rest_client.cpp` |
| Python client | RPC | — | `src/client/python/TuGraphClient/` |

## 9. Scaling summary

| Cost | Scaling | Where |
|---|---|---|
| Startup accept-readiness | O(N), sequential, blocks | `src/db/graph_manager.cpp:257-284` |
| Threads | 1/graph + 1/meta + 1/Bolt connection | `src/core/lmdb_store.cpp:105`, `src/server/bolt_handler.cpp:380` |
| FDs | 2/graph | LMDB env per graph |
| List graphs | O(N) per call | `src/db/graph_manager.cpp:213-217` |
| Every request | global `acl_lock_` read | `src/db/galaxy.cpp:125` |
| Every Cypher allocation | global `AllocatorManager` mutex | `src/cypher/monitor/memory_monitor_allocator.h:41` |
| Bolt connection | 1 OS thread, lifetime of connection | `src/server/bolt_handler.cpp:380-381` |
| Plan cache | per-thread, per-query-string; no sharing | `src/cypher/execution_plan/scheduler.cpp:74,170` |
| Shutdown | O(N) with blocking ref drain | `src/db/graph_manager.cpp:58-76` |

Components later phases will most likely need to change: `BoltServer`/
`BoltFSM` threading (move to a bounded worker pool), the single
`Galaxy::reload_lock_`/`acl_lock_`/`graphs_lock_` trio, `AllocatorManager`
locking, the `thread_local` plan cache, and the absent config surface for
graph-count/resource policy. See
[07-scalability-risks.md](07-scalability-risks.md).
