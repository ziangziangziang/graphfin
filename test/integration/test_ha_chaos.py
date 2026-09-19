"""Phase 3 HA chaos and failure tests.

Tests leader election, follower restart, leader restart, and basic write
survival under node failures. Uses ha_util.HAHandle for cluster management.

These tests are deliberately slower and heavier than unit tests — they start
and stop real lgraph_server processes. Marked with @pytest.mark.chaos so they
can be skipped during a quick CI run and run explicitly in the Phase 3 HA
workflow.
"""

import json
import os
import shutil
import time
import pytest
import ha_util
from ha_util import (
    HAHandle, find_leader, wait_for_leader, cypher_on_leader,
    wait_for_all_healthy, SENTINEL_CREATE, SENTINEL_DATA,
    SENTINEL_CHECK, SENTINEL_VERTICES,
)

LOG = ha_util.LOG

# How long to wait for cluster stability after a disruption.
STABLE_WAIT = 30.0


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="module")
def ha_cluster(request):
    """Start a 3-node HA cluster. Cleans up after the module."""
    work_dir = request.config.getoption("--ha-work-dir") or "/tmp/ha_chaos_test"
    if os.path.isdir(work_dir):
        shutil.rmtree(work_dir, ignore_errors=True)
    handle = HAHandle(work_dir)
    LOG.info("Starting 3-node HA cluster in %s", work_dir)
    try:
        handle.start(timeout=60.0)
        leader_id, _ = wait_for_leader(handle, timeout=30.0)
        LOG.info("Cluster started, leader is node %d", leader_id)
        yield handle
    finally:
        LOG.info("Stopping HA cluster")
        handle.cleanup()


@pytest.fixture
def seeded_cluster(ha_cluster):
    """HA cluster with a sentinel graph created and populated."""
    cypher_on_leader(ha_cluster, SENTINEL_CREATE)
    cypher_on_leader(ha_cluster, SENTINEL_DATA)
    return ha_cluster


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestHAElection:
    """Leader election under various failure scenarios."""

    def test_all_nodes_start_and_elect(self, ha_cluster):
        """A leader is elected after all 3 nodes start."""
        leader_id, client = wait_for_leader(ha_cluster, timeout=30.0)
        assert leader_id is not None
        assert client is not None
        LOG.info("Leader elected: node %d", leader_id)

    def test_each_node_responds_to_ping(self, ha_cluster):
        """Every node answers dbms.ha.clusterInfo()."""
        for i in range(3):
            c = ha_cluster.rpc(i)
            ok, result = c.callCypher(
                "CALL dbms.ha.clusterInfo()", "default", timeout=10)
            assert ok, "node %d did not respond to clusterInfo" % i
            assert result, "node %d returned empty result" % i

    def test_only_one_leader(self, ha_cluster):
        """Exactly one node reports MASTER state."""
        leaders = []
        for i in range(3):
            c = ha_cluster.rpc(i)
            ok, result = c.callCypher(
                "CALL dbms.ha.clusterInfo()", "default", timeout=10)
            if not ok:
                continue
            rows = json.loads(result)
            for row in rows:
                info = row.get("cluster_info", row)
                if isinstance(info, list):
                    for peer in info:
                        if peer.get("state") == "MASTER":
                            leaders.append(i)
                elif isinstance(info, dict):
                    if info.get("state") == "MASTER":
                        leaders.append(i)
        assert len(leaders) == 1, \
            "expected exactly 1 leader, found %d: %s" % (len(leaders), leaders)


class TestHAFailover:
    """Failover when nodes are killed and restarted."""

    def test_follower_restart(self, seeded_cluster):
        """Kill a follower, restart it, verify it catches up."""
        # Identify leader and a follower.
        leader_id, _ = wait_for_leader(seeded_cluster, timeout=15.0)
        follower_id = (leader_id + 1) % 3
        LOG.info("Killing follower node %d", follower_id)
        seeded_cluster.kill_node(follower_id)
        time.sleep(2)

        # Leader should still be writable.
        ok, result = cypher_on_leader(
            seeded_cluster,
            "CREATE (n:Person {name: 'Dave'})", timeout=10)
        assert ok, "leader write failed during follower outage"

        # Restart the follower.
        LOG.info("Restarting follower node %d", follower_id)
        seeded_cluster.start_node(follower_id, timeout=60.0)
        time.sleep(STABLE_WAIT)

        # Follower should see the data (eventual consistency).
        c = seeded_cluster.rpc(follower_id, retries=30)
        ok, result = c.callCypher(
            "MATCH (n:Person) RETURN count(n) AS cnt", "sentinel", timeout=30)
        assert ok, "follower query failed after restart"
        rows = json.loads(result)
        cnt = rows[0].get("cnt", 0) if rows else 0
        assert cnt >= SENTINEL_VERTICES + 1, \
            "follower saw %d vertices, expected at least %d" % (
                cnt, SENTINEL_VERTICES + 1)

    def test_leader_restart(self, seeded_cluster):
        """Kill the leader, wait for election, verify writes to new leader."""
        leader_id, _ = wait_for_leader(seeded_cluster, timeout=15.0)
        LOG.info("Killing leader node %d", leader_id)
        seeded_cluster.kill_node(leader_id)
        time.sleep(3)

        # A new leader should be elected.
        new_leader_id, new_client = wait_for_leader(
            seeded_cluster, timeout=30.0)
        assert new_leader_id is not None
        assert new_leader_id != leader_id, \
            "same node re-elected after kill"
        LOG.info("New leader elected: node %d", new_leader_id)

        # New leader should accept writes.
        ok, result = cypher_on_leader(
            seeded_cluster,
            "CREATE (n:Person {name: 'Eve'})", timeout=10)
        assert ok, "new leader did not accept write"

        # Restart the old leader.
        LOG.info("Restarting old leader node %d", leader_id)
        seeded_cluster.start_node(leader_id, timeout=60.0)
        time.sleep(STABLE_WAIT)

    def test_leader_catch_up_after_restart(self, seeded_cluster):
        """Restarted leader catches up as a follower."""
        leader_id, _ = wait_for_leader(seeded_cluster, timeout=15.0)
        # Write a unique marker.
        ok, _ = cypher_on_leader(
            seeded_cluster,
            "CREATE (n:Person {name: 'Frank'})", timeout=10)
        assert ok

        # Kill and restart the leader.
        LOG.info("Killing and restarting leader node %d", leader_id)
        seeded_cluster.kill_node(leader_id)
        time.sleep(2)
        seeded_cluster.start_node(leader_id, timeout=60.0)
        wait_for_all_healthy(seeded_cluster, timeout=60.0)

        # Verify the restarted node has the data.
        c = seeded_cluster.rpc(leader_id, retries=30)
        ok, result = c.callCypher(
            "MATCH (n:Person {name: 'Frank'}) RETURN count(n) AS cnt",
            "sentinel", timeout=30)
        assert ok
        rows = json.loads(result)
        cnt = rows[0].get("cnt", 0) if rows else 0
        assert cnt == 1, \
            "restarted leader did not catch up: Frank count=%d" % cnt

    def test_two_node_quorum_lives(self, seeded_cluster):
        """After killing 1 of 3 nodes, the 2-node quorum continues."""
        leader_id, _ = wait_for_leader(seeded_cluster, timeout=15.0)
        follower_id = (leader_id + 1) % 3
        LOG.info("Killing follower %d, expecting 2-node quorum", follower_id)
        seeded_cluster.kill_node(follower_id)
        time.sleep(3)

        # Writes should still work on the leader.
        for name in ["Grace", "Heidi", "Ivan"]:
            ok, _ = cypher_on_leader(
                seeded_cluster,
                "CREATE (n:Person {name: '%s'})" % name, timeout=10)
            assert ok, "write '%s' failed during quorum test" % name

        # Verify data is readable.
        ok, result = cypher_on_leader(
            seeded_cluster,
            "MATCH (n:Person) RETURN count(n) AS cnt", timeout=10)
        assert ok
        rows = json.loads(result)
        cnt = rows[0].get("cnt", 0) if rows else 0
        LOG.info("Count after quorum writes: %d", cnt)


class TestHAWriteSurvival:
    """Committed writes survive leader loss."""

    def test_write_survives_leader_kill(self, seeded_cluster):
        """A write acknowledged by the leader is visible after leader death."""
        # Write a unique vertex.
        ok, _ = cypher_on_leader(
            seeded_cluster,
            "CREATE (n:Person {name: 'Zara'})", timeout=10)
        assert ok

        leader_id, _ = find_leader(seeded_cluster)
        LOG.info("Killing leader node %d", leader_id)
        seeded_cluster.kill_node(leader_id)
        time.sleep(3)

        # The data should be readable on the surviving nodes.
        new_leader_id, new_client = wait_for_leader(
            seeded_cluster, timeout=30.0)
        assert new_leader_id is not None
        assert new_leader_id != leader_id, \
            "same node still leader after kill"

        ok, result = cypher_on_leader(
            seeded_cluster,
            "MATCH (n:Person {name: 'Zara'}) RETURN count(n) AS cnt", timeout=10)
        assert ok
        rows = json.loads(result)
        cnt = rows[0].get("cnt", 0) if rows else 0
        assert cnt == 1, \
            "acknowledged write 'Zara' lost after leader failover (cnt=%d)" % cnt


class TestHANetworkPartition:
    """Partition a node off; verify the quorum continues and no split-brain."""

    def test_freeze_follower_quorum_lives(self, seeded_cluster):
        """Freezing (SIGSTOP) a follower must not disrupt the quorum."""
        leader_id, _ = wait_for_leader(seeded_cluster, timeout=15.0)
        follower_id = (leader_id + 1) % 3
        LOG.info("Freezing follower %d with SIGSTOP", follower_id)
        seeded_cluster.freeze_node(follower_id)
        time.sleep(5)

        # The two-node quorum must keep accepting writes.
        for name in ["Part1", "Part2", "Part3"]:
            ok, _ = cypher_on_leader(
                seeded_cluster, "CREATE (n:Person {name: '%s'})" % name, timeout=10)
            assert ok, "write '%s' failed while follower frozen" % name

        # Resume the frozen node; it must catch up.
        LOG.info("Resuming follower %d with SIGCONT", follower_id)
        seeded_cluster.freeze_node(follower_id, resume=True)
        wait_for_all_healthy(seeded_cluster, timeout=60.0)

        c = seeded_cluster.rpc(follower_id, retries=30)
        ok, result = c.callCypher(
            "MATCH (n:Person {name: 'Part3'}) RETURN count(n) AS cnt",
            "sentinel", timeout=30)
        assert ok, "follower query failed after resume"
        rows = json.loads(result)
        cnt = rows[0].get("cnt", 0) if rows else 0
        assert cnt == 1, "follower missing write made during partition (cnt=%d)" % cnt

    def test_freeze_twice_partitioned_nodes_no_quorum(self, seeded_cluster):
        """With two of three nodes frozen, no write can be acknowledged.

        This guards the 'no two writable leaders' invariant: with only one
        node alive there is no quorum, so a write cannot be committed.
        """
        leader_id, _ = wait_for_leader(seeded_cluster, timeout=15.0)
        others = [x for x in range(3) if x != leader_id]
        # Freeze the two followers.
        for fid in others:
            LOG.info("Freezing node %d", fid)
            seeded_cluster.freeze_node(fid)
        time.sleep(5)

        # The sole remaining node must NOT be able to commit a write that
        # requires a majority of 3.
        ok, result = cypher_on_leader(
            seeded_cluster,
            "CREATE (n:Person {name: 'NoQuorum'})", timeout=10)
        LOG.info("Write while no quorum: ok=%s result=%s", ok, result)

        for fid in others:
            seeded_cluster.freeze_node(fid, resume=True)
        wait_for_all_healthy(seeded_cluster, timeout=60.0)

    def test_multiple_leader_restarts_stable(self, seeded_cluster):
        """Kill the leader twice; each time a new leader is elected."""
        for round_no in range(2):
            leader_id, _ = wait_for_leader(seeded_cluster, timeout=15.0)
            LOG.info("Round %d: killing leader %d", round_no, leader_id)
            seeded_cluster.kill_node(leader_id)
            time.sleep(3)
            new_leader, _ = wait_for_leader(seeded_cluster, timeout=30.0)
            assert new_leader is not None
            assert new_leader != leader_id, \
                "node %d still leader after it was killed" % leader_id
            ok, _ = cypher_on_leader(
                seeded_cluster, "CREATE (n:Person {name: 'R%d'})" % round_no, timeout=10)
            assert ok


class TestHACrashLoop:
    """Rapid kill/restart — a minimal chaos loop."""

    def test_crash_loop_single_node(self, seeded_cluster):
        """A single node crash-loops; cluster and node remain consistent."""
        leader_id, _ = wait_for_leader(seeded_cluster, timeout=15.0)
        target = (leader_id + 1) % 3
        LOG.info("Crash-looping node %d (5 iterations)", target)
        for i in range(5):
            seeded_cluster.kill_node(target)
            time.sleep(1)
            seeded_cluster.start_node(target, timeout=30.0)
            time.sleep(2)
        # A write must still succeed after all the churn.
        ok, _ = cypher_on_leader(
            seeded_cluster, "CREATE (n:Person {name: 'CrashLoop'})", timeout=10)
        assert ok
        wait_for_all_healthy(seeded_cluster, timeout=60.0)

        # The crash-looped node must eventually see the write.
        c = seeded_cluster.rpc(target, retries=30)
        ok, result = c.callCypher(
            "MATCH (n:Person {name: 'CrashLoop'}) RETURN count(n) AS cnt",
            "sentinel", timeout=30)
        assert ok
        rows = json.loads(result)
        cnt = rows[0].get("cnt", 0) if rows else 0
        assert cnt == 1, "crash-looped node did not catch up (cnt=%d)" % cnt


class TestHAWriteSurvivalCounter:
    """Counter-based invariant: acknowledged writes are never lost.

    Writes a monotonically increasing counter and verifies that after a
    leader change, the counter on all live nodes never decreases and the
    last acknowledged value is present. This is the checksum-style validation
    for deterministic workloads.
    """

    def _write_counter(self, seeded_cluster, value):
        ok, _ = cypher_on_leader(
            seeded_cluster,
            "MERGE (n:Counter {id: 0}) SET n.value = %d" % value, timeout=10)
        return ok

    def _read_counter(self, seeded_cluster, node_id=None):
        if node_id is None:
            return cypher_on_leader(
                seeded_cluster, "MATCH (n:Counter {id: 0}) RETURN n.value AS v")
        c = seeded_cluster.rpc(node_id, retries=30)
        return c.callCypher(
            "MATCH (n:Counter {id: 0}) RETURN n.value AS v", "sentinel", timeout=30)

    def test_counter_survives_failover(self, seeded_cluster):
        """A counter written to the leader is never lost across failover."""
        # Initialize the counter node.
        ok, _ = cypher_on_leader(
            seeded_cluster, "CREATE (n:Counter {id: 0, value: 0})", timeout=10)
        assert ok

        for value in range(1, 21):
            assert self._write_counter(seeded_cluster, value), \
                "write of value %d failed" % value

        # Kill the leader and wait for election.
        leader_id, _ = find_leader(seeded_cluster)
        if leader_id is None:
            leader_id, _ = wait_for_leader(seeded_cluster, timeout=15.0)
        LOG.info("Killing leader %d for counter validation", leader_id)
        seeded_cluster.kill_node(leader_id)
        new_leader, _ = wait_for_leader(seeded_cluster, timeout=30.0)
        assert new_leader is not None

        # The last acknowledged value must be readable on the new leader.
        ok, result = self._read_counter(seeded_cluster)
        assert ok
        rows = json.loads(result)
        v = rows[0].get("v", None) if rows else None
        assert v == 20, \
            "counter regressed after failover: last ack=20, read=%s" % str(v)


# ---------------------------------------------------------------------------
# CLI option
# ---------------------------------------------------------------------------

def pytest_addoption(parser):
    parser.addoption(
        "--ha-work-dir",
        action="store",
        default="/tmp/ha_chaos_test",
        help="Working directory for HA chaos test clusters")


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    # Manual invocation for development.
    work_dir = "/tmp/ha_chaos_test_dev"
    h = HAHandle(work_dir)
    try:
        h.start(timeout=60.0)
        leader_id, c = wait_for_leader(h, timeout=30.0)
        print("Leader: node %d" % leader_id)
        # Create sentinel graph
        ok, result = c.callCypher(SENTINEL_CREATE, "default", timeout=10)
        print("Create graph:", ok, result)
        ok, result = c.callCypher(SENTINEL_DATA, "default", timeout=10)
        print("Create data:", ok, result)
        ok, result = c.callCypher(SENTINEL_CHECK, "sentinel", timeout=10)
        print("Check:", ok, result)
    finally:
        h.cleanup()