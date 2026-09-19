# 08 — HA Durability and Consistency Guarantees

## 1. Scope

This document defines the durability and consistency semantics of TuGraph's
legacy HA mode (`--enable_ha true`), which uses Apache braft (Raft) as its
consensus engine. Bolt HA (`--bolt_raft_port`) has independent semantics that
are not covered here.

## 2. Terminology

| Term | Definition |
|---|---|
| **Leader** | The Raft node that holds the current term's leadership and accepts writes. Exactly one at any epoch. |
| **Follower** | A Raft node that replicates committed entries from the leader. |
| **Commit** | An entry is committed when a majority of the Raft group (leader + followers) have durably stored it. |
| **Acknowledged write** | A write whose Raft entry has been committed. The leader sends the response to the client only after commit. |
| **Raft log index** | Monotonically increasing integer stored in the Galaxy metadb (`GetRaftLogIndex`). Used for idempotent replay. |

## 3. Durability Guarantee

**A write is acknowledged to the client only after the corresponding Raft entry
has been committed to a majority of the replica group.**

### What this means

- If the leader responds `SUCCESS` to a write operation (Cypher write, schema
  change, graph create/delete, ACL modification, import, plugin operation),
  the effect is persisted on at least 2 out of 3 nodes.
- A subsequent leader failure will not lose that write — the new leader will
  have the committed entry and apply it.
- The system provides **no acknowledgements for uncommitted entries**. If the
  leader crashes before the entry reaches a majority, the client receives an
  error and should retry.

### Implementation (legacy HA)

```
HaStateMachine::DoRequest (is_write=true)
  └─ ReplicateAndApplyRequest
       └─ node_->apply(task)         // propose to braft
            └─ braft commits          // majority durably stores the log entry
                 └─ on_apply callback // every node applies the entry
                      └─ ApplyRequestDirectly
                           └─ galaxy_->SetRaftLogIndexBeforeWrite(index)
```

The `on_apply` callback implements exactly-once semantics via log index
deduplication:

```
if (iter.index() > galaxy_->GetRaftLogIndex()) {
    ApplyRequestDirectly(req, resp);
    galaxy_->SetRaftLogIndexBeforeWrite(iter.index());
} else {
    // Skip already-applied entry
}
```

## 4. Read Semantics

### 4.1 Leader reads

Reads executed on the leader see the latest committed state. The leader answers
all read requests directly from the local Galaxy.

### 4.2 Follower reads

Follower reads may return **stale** data if replication lag is present. A
follower that is sufficiently up-to-date serves reads locally; otherwise it
redirects the client to the leader.

### 4.3 Read-after-write

A client that receives a successful write acknowledgement and immediately reads
from the leader will see its own write. Reading from a follower may not —
clients that require read-after-write consistency should read from the leader.

## 5. Failure Semantics

### 5.1 Leader failure

1. The leader becomes unresponsive (crash, network partition, SIGKILL).
2. Followers detect the leader loss via the Raft election timeout
   (`election_timeout_ms`, default 500ms).
3. A new leader is elected from the remaining nodes.
4. The new leader has all committed entries. Uncommitted entries from the old
   term are discarded (standard Raft).
5. The client's in-flight write receives either a success (if committed) or an
   error (if the leader crashed before commit).

**Window of vulnerability:** `election_timeout_ms` (500ms) + network delay.

### 5.2 Follower failure

1. A follower crashes or becomes disconnected.
2. The leader continues to accept writes as long as a majority (≥2 nodes) remains.
3. The crashed follower misses all writes that occurred while it was down.
4. On restart, the follower catches up via log replay.
5. If the follower has fallen behind the compacted log prefix, a snapshot is
   installed first, then incremental log replay.

**Window of vulnerability:** None. The quorum is unaffected by single-follower
loss.

### 5.3 Network partition

- If a majority partition exists (≥2 nodes), they elect a leader and continue.
- If no majority exists (all three nodes in separate partitions, or a 1-2 split
  where the singleton does not have the leader), the group loses quorum and
  stops accepting writes.
- When the partition heals, the minority partition's nodes catch up via log
  replay or snapshot.

**Split-brain protection:** Standard Raft ensures that at most one leader
exists per term. A node in a minority partition cannot form a quorum and
cannot commit new entries.

### 5.4 Process crash mid-transaction

TuGraph uses LMDB transactions. If the server crashes before a transaction is
committed, the LMDB environment is left in a consistent state (atomicity
guaranteed by LMDB's MVCC). On restart, uncommitted changes are absent.

If the crash occurs during the Raft apply step (`ApplyRequestDirectly`) after
the entry is committed, the entry will be re-applied on restart via the Raft
log replay. The deduplication check (`iter.index() > committed_index`) ensures
idempotency.

## 6. Configuration Recommendations

| Parameter | Recommended | Notes |
|---|---|---|
| `election_timeout_ms` | 500–1000 | Lower = faster failover, higher = more stability under network jitter |
| `snapshot_interval_s` | 3600 (1h) | Trade-off: more frequent = faster recovery, less frequent = less I/O |
| `durable` | `true` | Ensures LMDB commits to disk; otherwise a power loss may lose the last few commits |
| `max_open_graphs` | 100–1000 | Phase 2 parameter; keep bounded to limit per-node memory |

## 7. Known Limitations (Roadmap)

| Limitation | Status | Notes |
|---|---|---|
| Single global Raft group | Open | All graphs share one consensus order; a slow graph can block others. Phase 7 (intra-graph sharding). |
| Bolt HA replication partial | Open | Only Bolt `Run` writes replicated; REST/RPC/admin writes bypass Bolt HA. Use legacy HA for full write-surface coverage. |
| Bolt HA snapshot recovery | **Mitigated** | Storage now keeps the etcd-raft snapshot contract (`ApplySnapshot`/`SetSnapshot`), `CheckReady` persists snapshots instead of fataling, and log GC is bounded by the slowest peer's match index so a lagging follower is never stranded behind the compacted prefix. A brand-new peer joining a fully-compacted group still needs a data-carrying snapshot (future work). |
| No Raft Prometheus metrics | **Fixed** | `tugraph_raft` gauge family now exposes term, commit index, applied index and leader state (Phase 3). |
| Bolt with legacy HA without Bolt Raft | **Fixed** | Startup now rejects `--bolt_port` + `--enable_ha` without `--bolt_raft_port` to prevent silent divergence. |

## 8. Document History

| Date | Author | Change |
|---|---|---|
| 2026-09-20 | Phase 3 | Initial definition |