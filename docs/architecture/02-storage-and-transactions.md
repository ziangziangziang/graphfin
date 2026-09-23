# 02 — Storage Engine, Transactions, Indexes and Schema

## 1. Storage engine

**Graph and metadata storage is embedded LMDB.** There is no RocksDB for graph
data. Backend selection is compile-time only (`src/core/kv_store.h:17-27`):

- `KvStore` = `LMDBKvStore` by default.
- `KvStore` = `MockKvStore` when built with `USE_MOCK_KV=1`
  (`Options.cmake:14-21`), used by tests.
- RocksDB is linked only for the Bolt-Raft log store (see
  [04](04-ha-raft-replication.md)), never for graph data.

The vendored LMDB source lives in `src/core/lmdb/` (`mdb.c`). The "mdb"
filenames you see are LMDB: `data.mdb` and `lock.mdb`.

### Interfaces

| Interface | Header |
|---|---|
| `KvStore`, `KvTable`, `KvTransaction`, `KvIterator` | `src/core/kv_engine.h:22-111` |
| `LMDBKvStore` | `src/core/lmdb_store.h:38-199` |
| `LMDBKvTable` | `src/core/lmdb_table.h:32-211` |
| `LMDBKvTransaction` | `src/core/lmdb_transaction.h:64-128` |
| `LMDBKvIterator` | `src/core/lmdb_iterator.h` |

Every table is an LMDB named DBI. The unnamed DB enumerates table names
(`src/core/lmdb_store.cpp:151-164`).

### Per-graph environment configuration

`LMDBKvStore::Open` (`src/core/lmdb_store.cpp:45-78`):

```cpp
mdb_env_set_mapsize(env_, db_size_);      // :56  default 4 TiB (see 01)
mdb_env_set_maxdbs(env_, 10000);          // :58
mdb_env_set_maxreaders(env_, 1200);       // :59
unsigned flags = MDB_NOMEMINIT | MDB_NORDAHEAD | MDB_NOSYNC;   // :60-65
mdb_env_open(env_, path_.c_str(), flags, 0664);
```

Consequences worth stating explicitly:

- **`MDB_NOSYNC` is always set.** LMDB never fsyncs. Durability is provided only
  by the optional custom WAL (`durable=true`) — see §4.
- **`maxdbs = 10000`** caps how many tables (labels, detached-property tables,
  index tables) one graph can have.
- **`maxreaders = 1200`** sizes the per-graph reader table in `lock.mdb`,
  independent of `LGRAPH_MAX_THREADS = 480`.
- Page size is the OS page size, capped at `MAX_PAGESIZE` 32 KiB
  (`src/core/lmdb/mdb.c:621,4840-4842`).
- LMDB keeps `NUM_METAS = 2` meta pages (`src/core/lmdb/mdb.c:1204`) and writes
  both on env creation (`:4872`), so a graph directory has an 8 KiB floor at
  4 KiB pages before any user data.

The meta store is the same class with a hard-coded **1 GiB** map
(`src/db/galaxy.cpp:554`).

## 2. On-disk data model

Graph records live in the `_graph_` table. Keys are `[5-byte vid][1-byte type]`;
values are packed per `src/core/graph_data_pack.h:55-80`, splitting into
`VERTEX_ONLY`, `OUT_EDGE`, and `IN_EDGE` records when a record exceeds
`NODE_SPLIT_THRESHOLD = 1000` bytes (`src/core/data_type.h:307`).

- vids are 5 bytes → `MAX_VID ≈ 2^40`
  (`src/core/data_type.h:294,303`).
- `MAX_LID ≈ 65533` (`src/core/defs.h:145`, `src/core/data_type.h:306`).

### Empty graph on disk

Per graph directory: `data.mdb` + `lock.mdb`, plus `_cpp_plugin_/`,
`_python_plugin_/` (created when plugins load), plus `_fulltext_index_/` only
with fulltext enabled. `data.mdb` contains two LMDB meta pages plus one page per
created table. `db_size` does **not** preallocate disk — LMDB uses sparse files.
The measured size per empty graph is in the Phase 0 baseline results.

## 3. Transactions

Three layers:

| Layer | Type | Location |
|---|---|---|
| Public/embedder API | `lgraph_api::Transaction` | `src/lgraph_api/lgraph_txn.cpp:38-52` |
| Graph semantics | `lgraph::Transaction` | `src/core/transaction.h:62-1133` |
| KV | `LMDBKvTransaction` | `src/core/lmdb_transaction.h:64-128` |

### Creation

`AccessControlledDB::CreateReadTxn/CreateWriteTxn/ForkTxn`
(`src/db/db.cpp:47-56`) check ACLs, then delegate to `LightningGraph`:

- `CreateWriteTxn(optimistic)` → `Transaction(false, optimistic, this)`
  (`src/core/lightning_graph.cpp:37-39`)
- `CreateReadTxn()` (`src/core/lightning_graph.cpp:3104-3108`)
- `ForkTxn()` is only valid for read transactions
  (`src/core/lightning_graph.cpp:3110-3113`)

The `Transaction` constructor (`src/core/transaction.cpp:134-149`):

```cpp
managed_schema_ptr_(db->schema_.GetScopedRef()),   // pin a schema snapshot
...
EnterTxn();
txn_ = read_only ? db->store_->CreateReadTxn()
                 : db->store_->CreateWriteTxn(optimistic);
```

`EnterTxn`/`LeaveTxn` use a **`static thread_local bool in_transaction_`**
shared by every `LightningGraph` (`src/core/lightning_graph.h:61-62`,
`src/core/transaction.cpp:119-132`). Nested transactions are forbidden, and a
thread can only be in one graph's transaction at a time. This is a per-thread,
cross-graph constraint, not a per-graph one.

### Commit and abort

`Transaction::Commit` (`src/core/transaction.cpp:362-393`):

1. close all iterators;
2. flush realtime per-label vertex/edge counts into `_meta_` (only when
   `enable_realtime_count` and not optimistic);
3. `txn_->Commit()` — the LMDB transaction;
4. commit fulltext index buffers if present;
5. release the schema reference and `LeaveTxn()`.

`Abort` (`src/core/transaction.cpp:395-405`); the destructor aborts if the
transaction was neither committed nor aborted (`src/core/transaction.cpp:226`).

Important: realtime counts and fulltext effects are **not** part of the KV
transaction, they are side effects applied around it.

### MVCC

Each stored value is `[8-byte write-txn-id][user value]`
(`src/core/lmdb_table.cpp:158-166` writes it; `:133` strips it on read). The
version id is the LMDB `mdb_txn_id`. A `for_update` read records the observed
version into the transaction's delta
(`src/core/lmdb_table.cpp:123-127`).

### Write model

- **One writer per graph, many readers**, enforced by LMDB's single-writer
  environment lock — not by a TuGraph lock. The `Graph` class states the caller
  must guarantee at most one writer (`src/core/graph.h:61-64`), and the WAL
  relies on that invariant (`src/core/wal.h:25-29`).
- `meta_lock_` (the per-graph `KillableRWLock`) is taken for schema, index,
  label and reload operations — it is the per-graph **DDL lock**, not the write
  lock for data.
- **Optimistic mode** (`optimistic_txn`, default false,
  `src/core/global_config.h:120`) lets many write transactions buffer changes in
  a `DeltaStore` and be serialized/validated at commit by the single validator
  thread (`src/core/lmdb_store.cpp:231-372`). Version mismatch → `MDB_CONFLICTS`
  → `TxnConflict`. Optimistic transactions cannot use `DUPSORT` tables
  (`src/core/lmdb_store.h:107-110`).

### Lock inventory

| Lock | Scope | Taken by |
|---|---|---|
| LMDB env writer lock | per graph | every non-optimistic write txn (`mdb_txn_begin`) |
| `LightningGraph::meta_lock_` | per graph | schema/index/label/reload (DDL) |
| `LMDBKvStore::mutex_` | per graph store | table open/delete (`src/core/lmdb_store.cpp:128,135,144`) |
| `LMDBKvStore::queue_mutex_`/`queue_cv_` | per graph store | optimistic commit queue |
| `Galaxy::reload_lock_` | **process-global** | state-machine request path; stop-the-world writer |
| `Galaxy::acl_lock_` | **process-global** | token validation, `OpenGraph`, ACL mutations |
| `Galaxy::graphs_lock_` | **process-global** | graph lookup and all lifecycle ops |
| `AllocatorManager` mutex | **process-global** | every Cypher/GQL allocation |

## 4. Durability and WAL

- LMDB envs are opened with `MDB_NOSYNC`, so LMDB itself never fsyncs.
- When `durable=true` (default **false**, `src/core/data_type.h:156`), a `Wal` is
  constructed per store (`src/core/lmdb_store.cpp:72-77`) writing
  `wal.log.<id>` and `dbi.log` in the graph directory
  (`src/core/wal.cpp:25-26,785-790`). Rotation defaults to 60 s, batch commit to
  10 ms (`src/core/lmdb_store.h:93-94`).
- `Wal::ReplayLogs()` replays on store open (`src/core/wal.cpp:412-432`, impl
  from `:459`), reading the WAL files and replaying up to the store's last
  committed op id.
- Each `LMDBKvStore` registers its last op id with a process-global atomic on
  open (`src/core/lmdb_store.cpp:66-71`; statics at `src/core/lmdb_store.h:181-194`).
  Note this atomic (`last_op_id_`) is **shared across all stores in the process**,
  including all graphs — a global serialization/ordering point.

## 5. Indexes

Types (`src/core/defs.h:102-107`): `vertex_index`, `edge_index`,
`composite_index`, `vertex_fulltext`/`edge_fulltext`, `vertex_vector_index`.

Index data lives in named LMDB tables inside the graph's `data.mdb`, named
`<label>_@lgraph@_<field>_@lgraph@_<type>`
(`src/core/index_manager.h:146-175`); the catalog is `_v_index_`.

- Adding a vertex label auto-creates the primary-field unique index and marks it
  ready immediately, since the label is empty
  (`src/core/lightning_graph.cpp:215-228`).
- Secondary and composite indexes are built **synchronously** by scanning
  records (`BlockingAddIndex`/`BlockingAddCompositeIndex`,
  `src/core/lightning_graph.cpp:2038+`, `:1926-2036`), with a fast path over
  detached property tables.
- **Opening a graph does not rebuild normal or composite indexes.**
  `IndexManager`'s constructor opens each index table and calls `SetReady()`
  unconditionally (`src/core/index_manager.cpp:47-51,60-64,91-96`).
- **Vector indexes are rebuilt in memory on every open** by scanning the label's
  detached property table (`src/core/index_manager.cpp:97-147`). This makes graph
  open cost O(rows) for any graph with a vector index — a direct startup-time
  scaling hazard layered on top of the O(N graphs) open loop.
- Fulltext indexes are external Lucene directories, opened not rebuilt, and only
  compiled with `ENABLE_FULLTEXT_INDEX` (default OFF, `Options.cmake:68-74`).

## 6. Schema

One serialized `Schema` per label, stored in `_v_schema_`/`_e_schema_` keyed by
`LabelId` (`src/core/schema_manager.h:62-84,185-233`; serialization
`src/core/schema.h:700-756`).

All schema mutation goes through **copy-on-write of a whole `SchemaInfo`** under
`meta_lock_`:

- `AddLabel`/`DelLabel` (`src/core/lightning_graph.cpp:133-268`, `:270-517`)
- `_AlterLabel` for field and edge-constraint changes
  (`src/core/lightning_graph.cpp:518-654`, callers `:728-1107`)

Unless `fast_alter_schema` is set, an alter rewrites **every affected record** in
one transaction (`src/core/lightning_graph.cpp:548-644`). The new `SchemaInfo` is
installed with `schema_.Assign(...)` (`:648`); the old version is collected once
no active transaction holds a `ScopedRef` (`src/core/managed_object.h:151-274`).
That ref-counted COW is the actual "schema versioning" mechanism.

There is **no schema version counter**. `SCHEMA_VERSION = 0`
(`src/core/data_type.h:314`) is defined but referenced nowhere. Only a
whole-database version (major.minor.patch) is stored and checked in the meta
store (`src/db/galaxy.cpp:747-786`).

`SchemaManager::schemas_` is a vector indexed by `LabelId`, including deleted
slots, so it grows with the highest label id ever used rather than the live label
count (`src/core/schema_manager.h:34-36,67`; slot reuse `:190-206`).

## 7. What scales with graph or label count

| Cost | Scaling | Where |
|---|---|---|
| Open LMDB envs | 1 per graph | `src/core/lightning_graph.cpp:3028` |
| Tables/DBIs | labels + detached properties + indexes per graph, capped 10000 | `src/core/lmdb_store.cpp:58` |
| Validator threads | 1 per graph + 1 for meta | `src/core/lmdb_store.cpp:105` |
| `last_op_id_` global atomic | shared by all stores | `src/core/lmdb_store.h:70,181-194` |
| Vector index open cost | O(rows) per vector-indexed label | `src/core/index_manager.cpp:97-147` |
| Schema alter cost | O(rows) unless `fast_alter_schema` | `src/core/lightning_graph.cpp:548-644` |
| `schemas_` vector | highest-ever label id, not live count | `src/core/schema_manager.h:34-36` |
