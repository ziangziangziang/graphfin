"""Acknowledged graph/series state survives a real HA leader loss.

This baseline kills after acknowledgements. In-flight atomicity, partitions and
snapshot interruption remain separate gates in docs/testing/post-merge.md.
"""

import json
import time

import pytest

from ha_util import (HAHandle, SENTINEL_GRAPH,
                     wait_for_leader, wait_for_all_healthy)
from test_merge_series import CASES, seed, verify


LEADER_REDIRECTS = ("Not a leader", "no leader", "not the leader")


def leader_write(cluster, script, graph="default", timeout=15, deadline_s=150):
    """Execute one write against the current HA leader.

    The single-connection Python client sends to its connected node only, so
    every attempt re-discovers the leader and connects to it directly. Only
    redirect/connection failures are retried; any other server error is
    returned immediately so product failures stay visible.
    """
    deadline = time.monotonic() + deadline_s
    last = "no attempt made"
    while time.monotonic() < deadline:
        try:
            _, client = wait_for_leader(cluster, timeout=15.0)
        except Exception as exc:
            last = exc
            time.sleep(1)
            continue
        try:
            try:
                result = client.callCypherToLeader(script, graph, timeout=timeout)
            except Exception as exc:
                last = exc
                time.sleep(1)
                continue
            if result[0]:
                return result
            message = str(result[1])
            if any(marker in message for marker in LEADER_REDIRECTS):
                last = result
                time.sleep(1)
                continue
            return result
        finally:
            try:
                client.logout()
            except Exception:
                pass
    return False, last


def create_graph_on_leader(cluster, name, deadline_s=150):
    ok, result = leader_write(
        cluster, "CALL dbms.graph.createGraph('%s', 'merged series test', 1)"
        % name, graph="default", deadline_s=deadline_s)
    assert ok or "already" in str(result).lower(), result


def wait_for_graph_visible(cluster, name, timeout=60):
    """Poll the leader until the new graph is listed; closes the create-seed race."""
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            _, client = wait_for_leader(cluster, timeout=15.0)
        except Exception as exc:
            last = exc
            time.sleep(1)
            continue
        try:
            ok, result = client.callCypherToLeader(
                "CALL dbms.graph.listGraphs()", "default", timeout=10)
        except Exception as exc:
            last = exc
            try:
                client.logout()
            except Exception:
                pass
            time.sleep(1)
            continue
        try:
            client.logout()
        except Exception:
            pass
        if ok:
            try:
                rows = json.loads(result)
                names = [row.get("graph_name", row.get("name"))
                         for row in rows if isinstance(row, dict)]
                if name in names:
                    return
                last = names
            except (ValueError, TypeError) as exc:
                last = exc
        else:
            last = result
        time.sleep(1)
    pytest.fail("graph %r not visible on leader within deadline: %r" % (name, last))


@pytest.mark.parametrize("label,edge", CASES)
def test_series_reconciles_on_every_replica_after_leader_loss(tmp_path, label, edge):
    cluster = HAHandle(str(tmp_path / "ha"), extra_args=["--durable", "true"])
    try:
        cluster.start()
        wait_for_all_healthy(cluster)
        # Create the logical graph through the explicit HA leader path and
        # wait until it is listed before seeding; the generic follower-redirect
        # helper cannot target graph-management writes reliably.
        create_graph_on_leader(cluster, SENTINEL_GRAPH)
        wait_for_graph_visible(cluster, SENTINEL_GRAPH)
        writer = LeaderWriter(cluster)
        seed(writer, SENTINEL_GRAPH, label, edge, 100, create=False)
        # Wait for exact values, not just membership or matching row counts.
        reconcile(cluster, label, edge)
        leader, client = wait_for_leader(cluster)
        client.logout()
        cluster.kill_node(leader)
        new_leader, client = wait_for_leader(cluster)
        assert new_leader != leader
        verify(ReplicaReader(client, cluster.rpc_ports[new_leader]),
               SENTINEL_GRAPH, label, edge, 100)
        client.logout()
        cluster.start_node(leader)
        wait_for_all_healthy(cluster)
        reconcile(cluster, label, edge)
    finally:
        # Keep files/logs available to the evidence runner.
        for node in cluster.nodes:
            node.kill()


def reconcile(cluster, label, edge, timeout=90):
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = {}
        for replica in range(3):
            c = None
            try:
                c = cluster.rpc(replica, retries=5)
                verify(ReplicaReader(c, cluster.rpc_ports[replica]),
                       SENTINEL_GRAPH, label, edge, 100)
            except Exception as exc:
                last[replica] = str(exc)
            finally:
                if c is not None:
                    c.logout()
        if not last:
            return
        time.sleep(0.5)
    pytest.fail("replicas did not reconcile within deadline: %r" % last)


class LeaderWriter:
    """Use the SDK's explicit leader path, not its cached query classification."""

    def __init__(self, cluster):
        self.cluster = cluster

    def callCypher(self, script, graph="default"):
        last = None
        deadline = time.monotonic() + 150
        while time.monotonic() < deadline:
            try:
                _, c = wait_for_leader(self.cluster, timeout=15.0)
            except Exception as exc:
                last = (False, exc)
                time.sleep(1)
                continue
            try:
                try:
                    result = c.callCypherToLeader(script, graph, timeout=15)
                except Exception as exc:
                    last = (False, exc)
                    time.sleep(1)
                    continue
                if result[0] or not any(
                        message in str(result[1])
                        for message in ("Not a leader", "No such graph",
                                        "no leader", "not the leader")):
                    return result
                last = result
            finally:
                try:
                    c.logout()
                except Exception:
                    pass
            time.sleep(1)
        return last if last is not None else (False, "leader write deadline exceeded")


class ReplicaReader:
    """Pin reads to a physical replica; generic HA reads may be load balanced."""

    def __init__(self, client, port):
        self.client = client
        self.url = "127.0.0.1:%d" % port

    def callCypher(self, script, graph="default"):
        return self.client.callCypher(script, graph, timeout=10, url=self.url)
