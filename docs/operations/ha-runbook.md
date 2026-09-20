# TuGraph HA Operations Runbook

Operational procedures for a 3-node HA replica group (leader + two followers).
See `docs/architecture/04-ha-raft-replication.md` for the topology and
`08-ha-durability-guarantees.md` for the consistency contract.

## 0. Health check

```bash
# Per node, confirm the cluster view (leader, peers, state):
#   CALL dbms.ha.clusterInfo()
# Prometheus: tugraph_raft{metric="is_leader"|"current_term"|"commit_index"}.
```
Signs of trouble: no `MASTER` peer; `current_term` climbing repeatedly
(elections flapping); `commit_index` not advancing; a peer stuck `OFFLINE`.

## 1. Leader failover (automatic)

- The group re-elects within `ha_election_timeout_ms`. No operator action.
- Validate: `dbms.ha.clusterInfo()` shows exactly one `MASTER`; a probe write
  succeeds; the last acknowledged value is readable.
- If no leader after several timeouts: check that **at least 2 of 3** nodes are
  running and can reach each other (no partition, no disk-full).

## 2. Replace a failed replica

1. Confirm the other two nodes are healthy (quorum = 2).
2. Decommission the dead node (below) if it still owns its address.
3. Start a fresh node with the **same `ha_conf`, same `bolt_raft_node_id` (Bolt
   HA) / same peer address (legacy HA), empty data dir if it must re-sync**.
4. Watch it join and catch up (`Node joined the group as follower`; follower
   `commit_index` converges). Snapshot install is used if it fell behind the
   compacted prefix (legacy braft HA).

## 3. Decommission a node

- **Graceful**: stop the node; it transfers leadership first (`LeaveGroup`).
- **Force**: kill the process. The leader marks it OFFLINE after
  `ha_node_offline_ms` and removes it after `ha_node_remove_ms`. Never remove
  two of three nodes — that destroys quorum.

## 4. Backup / restore

- Whole-server snapshot: `dbms.backup.createSnapshot()` and `lgraph_backup`.
- **Restore is off-line**: stop the group, restore the data dirs, restart. The
  group re-forms from the restored state; all three nodes must be restored to
  the **same** snapshot.
- After restore, verify `dbms.ha.clusterInfo()` and a probe read.

## 5. Disk-full handling

- Writes fail once LMDB cannot grow. Free space or raise the per-graph
  `db_size`. The leader will not commit; followers fall behind. Do not delete
  `data.mdb` — back up and restore instead.
- Raft logs: legacy HA `ha_log_dir`, Bolt HA `bolt_raft_logstore_path`.

## 6. Migration troubleshooting (Phase 5)

- Stuck migration: inspect the migration state (`dbms.cluster` / control API),
  then `retryMigration` or `cancelMigration`. Migration is restartable and
  idempotent; a failed cutover rolls back while the source is still writable.
- Never allow two writable copies: after cutover the placement version is
  bumped and the destination is the only writable location; the source accepts
  writes only for the old version and **must be fenced** (receiver-side fence).

## 7. Capacity expansion

1. Provision new shards (each a fresh 3-node replica group).
2. Register them (`dbms.cluster.registerShard`).
3. New graphs auto-place onto the least-loaded shard; existing graphs move via
   Phase 5 migration or `drainShard` to rebalance.
4. Increase `max_open_graphs` / host memory only after measuring.

## 8. Escalation signals

| Signal | Likely cause | Action |
|---|---|---|
| No leader > 3× election timeout | loss of quorum / partition | restore ≥2 connected nodes |
| `current_term` rising fast | tight election timeout / CPU starvation | raise `ha_election_timeout_ms`, reduce load |
| Follower permanently behind | log GC past its prefix | verify safe-GC bounds; re-provision from snapshot |
| `commit_index` not advancing | disk full / majority down | free space; restore quorum |
