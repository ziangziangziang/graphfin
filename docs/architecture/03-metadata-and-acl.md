# 03 — Graph Metadata, Server Metadata and ACL

TuGraph metadata is split across **two locations**, and knowing which is which is
essential for any multi-graph scaling work:

- **Per-graph metadata** lives inside each graph's own LMDB environment
  (`<db_dir>/<subdir>/data.mdb`).
- **Server-wide metadata** (graph registry, users, roles, whitelist, version)
  lives in the single shared meta store `<db_dir>/.meta/data.mdb`.

There is no separate metadata database per graph and no "meta vertex label".
Graph name → config mapping is a table in the shared meta store.

## 1. The meta store

Created by `Galaxy`'s constructor (`src/db/galaxy.cpp:40-42`) and re-opened in
`Galaxy::ReloadFromDisk` (`src/db/galaxy.cpp:554`) with a hard-coded map size:

```cpp
store_.reset(new LMDBKvStore(config_.dir + "/.meta", 1 << 30, false, true));
```

1 GiB, non-durable. Tables opened there (`src/db/galaxy.cpp:557-572`):

| Table | Purpose | Constant |
|---|---|---|
| `_meta_` | server config flags + TuGraph version | `src/core/defs.h:113-122` |
| `_graph_config_table_` | graph name → serialized `DBConfig` | `GRAPH_CONFIG_TABLE_NAME` `src/core/defs.h:110` |
| `_ip_whitelist_` | allowed client IPs | `IP_WHITELIST_TABLE` `src/core/defs.h:89` |
| `_user_table_` | user → `UserInfo` | `USER_TABLE_NAME` `src/core/defs.h:108` |
| `_role_table_` | role → `RoleInfo` | `ROLE_TABLE_NAME` `src/core/defs.h:109` |

`Galaxy::ReloadFromDisk` performs, in order
(`src/db/galaxy.cpp:540-576`): take the `reload_lock_` **write** lock, close all
graphs, destroy `graphs_`/`acl_`/`store_`, reopen `.meta`, `CheckTuGraphVersion`,
`LoadConfigTable`, `LoadIpWhitelist`, then `GraphManager::Init` and
`AclManager::Init`.

Because the graph registry lives in one shared LMDB env with a fixed 1 GiB map,
the meta store is a **single serialization point for graph registration**: every
graph create/delete/modify and every ACL change writes through the same
transaction and the same env. At 100k graphs the registry alone becomes a
contention and size concern.

## 2. Per-graph metadata

Inside a graph's own `data.mdb`:

| Table / key | Contents | Where |
|---|---|---|
| `_meta_` table | `_db_secret_`, `_next_vid_`, and per-label `_vertex_count_<lid>` / `_edge_count_<lid>` | `src/core/defs.h:125-130`; used `src/core/lightning_graph.cpp:3115-3140`, `src/core/graph.h:96-113,364-463` |
| `_graph_` | all vertex/edge records | `src/core/graph.h:31-51` |
| `_v_schema_`, `_e_schema_` | label → serialized `Schema` | `src/core/schema_manager.h:62-84` |
| `_v_index_` | index catalog | `src/core/index_manager.h:36-132` |
| `_blob_` | large blobs | `src/core/blob_manager.h:30-149` |
| `_vertex_property_<label>`, `_edge_property_<label>` | detached property tables | `src/core/defs.h:133-134` |
| `_cpp_plugin_`, `_python_plugin_` | plugin catalogs | `src/core/defs.h:95-96` |

The `_db_secret_` is checked with `CheckDbSecret` on every open
(`src/core/lightning_graph.cpp:3115-3140`; called
`src/db/graph_manager.cpp:113,282`). It is derived from the generated subdirectory
name, which is how TuGraph detects a graph directory that has been swapped or
copied incorrectly.

`_next_vid_` and the per-label counts are hot metadata written on vertex/edge
creation and on commit when `enable_realtime_count` is set.

## 3. ACL: users, roles, permissions

ACL is **server-wide**, not per graph. Tables `_user_table_` and `_role_table_`
live in `.meta` and are loaded by `AclManager::ReloadFromDisk`
(`src/db/acl.cpp:555-592`).

### Data model

`UserInfo` (`src/db/acl.h:44-78`): set of roles, MD5 password hash, description,
auth method, disabled flag, and a **memory limit**.

`RoleInfo` (`src/db/acl.h:139-163`):

- `disabled`, `is_primary`, `desc`
- `graph_access`: `graph → AccessLevel`
- `field_access`: `graph → (label, field) → FieldAccessLevel`

So a role's permission footprint is **O(graphs)** per role, and `UserInfo`
carries role memberships. The in-memory user cache merges roles into a single
`graph → AccessLevel` map plus an `is_admin` flag
(`src/db/acl.cpp:76-107`, `src/db/acl.h:193-203`).

Memory therefore scales as **O(users × graphs)** for the merged cache and
**O(roles × graphs)** for role definitions. With 10k+ graphs this is a real
memory and copy-cost factor, especially because every graph create/delete
copy-constructs the whole `AclManager` (see §5).

### Built-ins

| Thing | Value | Where |
|---|---|---|
| Admin user | `admin` | `src/core/defs.h:78` |
| Default admin password | `73@TuGraph` | `src/core/defs.h:79` |
| Admin role | `admin` | `src/core/defs.h:80` |
| Meta-graph marker | `@meta_graph@` | `src/core/defs.h:82` |

A user is an admin iff one of its roles has `FULL` access on `@meta_graph@`
(`src/db/acl.cpp:71-74,549-553`). If the tables are empty on load, the built-in
admin user/role are seeded (`src/db/acl.cpp:560-577`).

The password hash is MD5 with a **static salt** `"We love salt."`
(`src/db/acl.cpp:21-23`; `src/core/defs.h:84`).

### Tokens

`TokenManager` (`src/db/token_manager.h:37-99`) issues JWTs.
`Galaxy::GetUserToken` is the REST login path
(`src/restful/server/rest_server.cpp:1537`).

Important property: **token → user bindings are in-memory only**
(`src/db/acl.cpp:915-960`). They are not persisted; JWTs are re-decoded after a
restart. The token map is bounded by `MAX_TOKEN_NUM_PER_USER = 10000`
(`src/db/galaxy.h:40`, `src/db/acl.h:198`), so worst-case token memory is
O(users × 10000) — bounded but large.

`Galaxy::ParseAndValidateToken` takes `acl_lock_` in read mode
(`src/db/galaxy.cpp:125`), and it runs on **every authenticated request**. This
makes `acl_lock_` one of the hottest global locks in the system.

### Access enforcement

`Galaxy::OpenGraph` resolves the caller's `AccessLevel` and wraps the graph in an
`AccessControlledDB` (`src/db/galaxy.cpp:380-388`; `src/db/db.h:24-33,166-180`).
Server-side use is in `StateMachine::ApplyGraphQueryRequest`-adjacent code
(`src/server/state_machine.cpp:434-461`). Enforcement happens at graph open, per
transaction, not once per connection.

Field-level ACL (`dbms.security.modRoleFieldAccessLevel`,
`src/cypher/procedure/procedure.cpp:2588-2631`) is stored in
`RoleInfo::field_access`; enforcement is limited compared to graph-level access.

## 4. Mutation path and copy-on-write

All ACL mutation funnels through `Galaxy::ModifyACL`, which copy-on-writes the
entire `AclManager` under `acl_lock_` (`src/db/galaxy.cpp:268-279`).

Grant/revoke entry points (`Galaxy::CreateUser`, `ModRole`, role/graph access
changes) therefore cost O(users + roles × graphs) per call.

## 5. Why this matters for graph count

Graph create/delete copy the `AclManager` as well as the `GraphManager`:

```cpp
// src/db/galaxy.cpp:192-193  (CreateGraph)
std::unique_ptr<AclManager> acl_new(new AclManager(*acl_));
std::unique_ptr<GraphManager> gm_new(new GraphManager(*graphs_));

// src/db/galaxy.cpp:217-218  (DeleteGraph) — identical
```

Combined with `acl_new->AddGraph(...)` / `acl_new->DelGraph(...)`, each graph
lifecycle operation:

1. copies every user's merged `graph → AccessLevel` map, and
2. copies every role's `graph_access` map,

under the global `acl_lock_` + `graphs_lock_` write locks. This is
O(users × graphs) per operation and O(N²) across a bulk graph-creation run. It is
the most expensive part of graph creation once any non-trivial ACL exists.

## 6. Scaling summary

| Cost | Scaling | Where |
|---|---|---|
| Graph registry size | 1 row per graph in a 1 GiB meta env | `src/db/graph_manager.cpp:50-56` |
| Graph lifecycle op | O(users × graphs) ACL copy + O(graphs) GraphManager copy | `src/db/galaxy.cpp:192-193,217-218` |
| ACL in-memory cache | O(users × graphs) | `src/db/acl.cpp:76-107` |
| Role definitions | O(roles × graphs) | `src/db/acl.h:139-163` |
| Token bindings | O(users), non-persistent | `src/db/acl.cpp:915-960` |
| Every authenticated request | global `acl_lock_` read + token validation | `src/db/galaxy.cpp:125` |
| `ReloadFromDisk` | closes and reopens **everything** | `src/db/galaxy.cpp:540-576` |

Components likely to need change in later phases: `AclManager` (avoid full copy
per graph op; move graph_access out of per-role maps), the graph registry
(partition/shard the config table rather than one shared env), and `Galaxy`'s
single `acl_lock_`/`graphs_lock_` pair. See
[07-scalability-risks.md](07-scalability-risks.md).
