# 09 — Write-Path Audit: Replication Coverage

## 1. Scope

Every write path through the server must enter a Raft replication path when
the server runs in HA mode. Having two independent Raft subsystems (legacy
braft HA and Bolt etcd-raft) creates a risk of silent divergence: if a write
enters one replication path but not the other, replicas may diverge.

This document audits every write surface and determines which replication
path it enters.

## 2. Replication Paths

| Path | Enabled by | Replicated unit | Raft engine |
|---|---|---|---|
| **Legacy HA** | `--enable_ha true` | whole `LGraphRequest` protobuf | braft |
| **Bolt HA** | `--bolt_port > 0` AND `--bolt_raft_port > 0` | raw Bolt `Run` payload | vendored etcd-raft-cpp |

The two subsystems are independent. They can be active simultaneously and
share no state.

## 3. Write Surface Coverage

### 3.1 RPC writes (all `LGraphRequest` types)

| Request type | Replicated by legacy HA? | Replicated by Bolt HA? |
|---|---|---|
| `GraphApiRequest` (add/del vertex, edge, label, index) | Yes (`DoRequest` → `ReplicateAndApplyRequest`) | No |
| `GraphQueryRequest` (Cypher write) | Yes (same path) | Only if `bolt_raft_port > 0` |
| `GraphRequest` (create/delete graph) | Yes | No |
| `AclRequest` (user/role management) | Yes | No |
| `ConfigRequest` (modify config) | Yes | No |
| `PluginRequest` (load/call/unload plugins) | Yes | No |
| `ImportRequest` (online import) | Yes | No |
| `SchemaRequest` (schema modification) | Yes | No |
| `RestoreRequest` (restore from backup) | Yes | No |

**Path:** `RPCService` → `StateMachine::HandleRequest` → `HaStateMachine::DoRequest`
→ `ReplicateAndApplyRequest` → `node_->apply(task)` (braft commit).

### 3.2 HTTP/REST writes

HTTP requests are converted to `LGraphRequest` protos and processed by the
same `StateMachine::HandleRequest` → `HaStateMachine::DoRequest` path. All
HTTP write operations therefore enter legacy HA replication.

### 3.3 Bolt writes

Two cases:

**Case A: Bolt HA active (`--bolt_raft_port > 0`).**
The `BoltFSM` handler checks `ReadOnlyCypher`; for write statements it calls
`RaftDriver::ProposeRaftRequest` which proposes the raw Bolt Run payload
through the vendored etcd-raft system (`bolt_handler.cpp:282-298`). On apply,
`ApplyRaftRequest` re-parses and re-executes the Cypher on every node
(`bolt_handler.cpp:115-163`).

**This replication is completely independent of legacy HA.** A follower will
receive and apply the write through Bolt HA's apply callback even if legacy
HA is inactive.

**Case B: Bolt HA inactive, legacy HA active (`--enable_ha true`, `--bolt_raft_port 0`).**
The `BoltFSM` handler executes the Cypher directly via
`Scheduler::Eval()` (`bolt_handler.cpp:301-303`) without calling
`HaStateMachine::DoRequest` or `ReplicateAndApplyRequest`.

**Bolt writes are NOT replicated by legacy HA in this configuration.**

## 4. Identified Gaps

### Gap 1 — Bolt writes escape legacy HA replication

When `--enable_ha true` and `--bolt_raft_port 0`, Bolt protocol writes are
not replicated. Only the node that receives the Bolt connection applies the
write; other replicas in the Raft group silently diverge.

**Impact:** Corrupts replica consistency if Bolt is used with legacy HA but
without Bolt HA. The divergence is silent (no error is raised).

**Fix:** Either:
1. Require `bolt_raft_port > 0` whenever `bolt_port > 0` and `enable_ha true`;
2. Route Bolt writes through `HaStateMachine::DoRequest` (proposing the
   Cypher as a `GraphQueryRequest` via braft) when legacy HA is active.

**Recommended:** Option 1 is simpler and safer. Add a startup validation.

### Gap 2 — Bolt HA does not replicate non-Cypher writes

Bolt HA only replicates the Bolt `Run` message (raw Cypher text). The
following operations, even if triggered from Bolt, are NOT replicated by
Bolt HA:

- Schema operations (`CREATE/DROP INDEX`, schema changes via Bolt)
- Graph lifecycle (`CREATE/DROP GRAPH`)
- ACL changes

**Impact:** If Bolt HA is used as the sole replication mechanism, these
operations diverge between replicas.

**Mitigation:** These operations are typically performed via REST/RPC,
which ARE replicated by legacy HA. In a mixed configuration (both HA
modes enabled), the combination covers all surfaces.

### Gap 3 — `lgraph_import --online` with Bolt HA

Online import uses the RPC path, which is replicated by legacy HA but
NOT by Bolt HA. If Bolt HA is operating without legacy HA, online imports
diverge.

## 5. Startup Validation

Add the following checks to `lgraph_server.cpp::Start`:

```cpp
// Gap 1 prevention: Bolt with legacy HA requires Bolt HA.
if (config_->bolt_port > 0 && config_->enable_ha && config_->bolt_raft_port == 0) {
    LOG_ERROR() << "Bolt port requires bolt_raft_port when running in HA mode. "
                << "Set --bolt_raft_port to a valid port.";
    return -1;
}
```

## 6. Mutation-ordering contract (decision)

**Decision: one authoritative mutation order per shard, and only one supported
replication path may be *enabled for writes* on a given deployment.**

Rationale (review finding): enabling legacy HA (RPC/REST writes) and Bolt HA
(`Run` writes) at the same time gives two independent Raft logs. A schema change
through RPC and a dependent write through Bolt can then enter different logs
with **no defined relative order**, so replicas can apply them inconsistently.
The startup guard (Gap 1) prevents the one clearly-unsafe combination, but does
not establish a common order when both are enabled.

Rules until the paths are unified:

1. A given shard/deployment selects exactly one write replication path:
   legacy HA **or** Bolt HA, never both for writes.
2. Every supported write surface on that deployment feeds the selected path:
   - legacy HA: all `LGraphRequest` writes (RPC, REST, import, admin, Bolt via
     `HaStateMachine::DoRequest`).
   - Bolt HA: the Bolt `Run` path **and** the RPC/REST/admin surfaces that
     mutate the same graphs (currently a gap — see §4 Gap 2/3).
3. A deployment that cannot route every write surface through its chosen path
   must not enable HA.

The startup validation should be extended from "Bolt requires Bolt HA" to
"exactly one write-replication path is enabled, and all write surfaces are
covered by it", rejecting mixed configurations.

Consequence for Phase 4: because a shard is the unit of replication, this
contract is enforced **per shard**, so whole-graph sharding is built on a
single-order-per-shard foundation rather than on the unsettled two-path
configuration. Unifying the two subsystems (one path for all surfaces) remains
the long-term goal and must not be deferred past Phase 6.

## 7. Recommendations

| Priority | Action | Phase |
|---|---|---|
| P0 | Enforce "exactly one write-replication path, all surfaces covered" at startup | Phase 3/4 |
| P0 | Define and document the per-shard mutation order and ambiguous-write/retry semantics | Phase 3/4 |
| P1 | Make Bolt HA cover non-`Run` surfaces, or route Bolt writes through the state machine | Phase 4 |
| P2 | Unify the two Raft subsystems into one authoritative mutation order | Phase 6 (was Phase 7) |

## 8. Document History

| Date | Author | Change |
|---|---|---|
| 2026-09-20 | Phase 3 | Initial write-path audit |
| 2026-09-20 | Phase 3/4 review | Mutation-ordering decision; single-path-per-shard contract |