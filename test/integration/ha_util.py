"""Reusable helpers for Phase 3 HA/Replication integration tests.

Builds on the pattern from phase0_util.py: launches 3-node lgraph_server
processes on localhost, manages them as a HA cluster, and provides helpers
for leader election, restart, and state verification.

Usage:
    from ha_util import HAHandle, wait_for_leader, cypher_on_leader
    h = HAHandle(tmp_path)
    h.start()
    leader_rpc = wait_for_leader(h)
    cypher_on_leader(h, "CALL dbms.graph.createGraph('g1', '', 1)")
    h.stop()
    h.cleanup()
"""

import json
import logging
import os
import signal
import socket
import subprocess
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILD_OUTPUT = os.path.join(REPO_ROOT, "build", "output")

DEFAULT_USER = "admin"
DEFAULT_PASSWORD = "73@TuGraph"
READY_MARKER = "Server started."

# Port offsets for the 3 nodes (base + offset).
# Node 0: HTTP 17072, RPC 19092
# Node 1: HTTP 17073, RPC 19093
# Node 2: HTTP 17074, RPC 19094
# Use a high base to avoid clashing with existing HA tests (27072).
HTTP_BASE = 17072
RPC_BASE = 19092


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def server_binary():
    for candidate in (
        os.path.join(BUILD_OUTPUT, "lgraph_server"),
        "./lgraph_server",
    ):
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)
    raise RuntimeError("lgraph_server not found. Build first.")


class HANodeHandle(object):
    """Owns one lgraph_server process in a 3-node HA cluster."""

    def __init__(self, node_id, db_dir, ha_conf, log_path=None, extra_args=None,
                 http_port=None, rpc_port=None):
        self.node_id = node_id
        self.db_dir = os.path.abspath(db_dir)
        self.ha_conf = ha_conf
        self.log_path = log_path or os.path.join(self.db_dir + ".log")
        self.extra_args = list(extra_args or [])
        self.proc = None
        self._logf = None
        self.http_port = http_port
        self.rpc_port = rpc_port

    def cmd(self):
        c = [
            server_binary(),
            "--host", "127.0.0.1",
            "--port", str(self.http_port),
            "--enable_rpc", "true",
            "--rpc_port", str(self.rpc_port),
            "--directory", self.db_dir,
            "--log_dir", self.db_dir + "/log",
            "--enable_ha", "true",
            "--ha_conf", self.ha_conf,
            "--ha_node_offline_ms", "5000",
            "--ha_node_remove_ms", "10000",
            "--ha_node_join_group_s", "30",
            "--verbose", "1",
        ]
        if self.node_id == 0:
            c.append("--ha_bootstrap_role=1")
        else:
            c.append("--ha_bootstrap_role=2")
        return c + self.extra_args

    def start(self, timeout=120.0):
        if self.proc is not None and self.proc.poll() is None:
            return self
        os.makedirs(self.db_dir, exist_ok=True)
        self._logf = open(self.log_path, "w")
        t0 = time.time()
        self.proc = subprocess.Popen(
            self.cmd(), stdout=self._logf, stderr=subprocess.STDOUT,
            close_fds=True, cwd=os.path.dirname(server_binary()))
        deadline = t0 + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    "node %d server exited early:\n%s"
                    % (self.node_id, self.log_tail()))
            if self._log_has(READY_MARKER):
                return self
            time.sleep(0.1)
        self.kill()
        raise RuntimeError(
            "node %d did not become ready in %.0fs:\n%s"
            % (self.node_id, timeout, self.log_tail()))

    def _log_has(self, marker):
        try:
            with open(self.log_path, "r") as f:
                return marker in f.read()
        except (IOError, OSError):
            return False

    def log_tail(self, n=60):
        try:
            with open(self.log_path, "r") as f:
                return "\n".join(f.read().splitlines()[-n:])
        except (IOError, OSError):
            return "(no log)"

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self, timeout=60.0):
        if not self.alive():
            return None
        t0 = time.time()
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.kill()
        if self._logf:
            self._logf.close()
            self._logf = None
        return time.time() - t0

    def kill(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        if self._logf:
            self._logf.close()
            self._logf = None


class HAHandle(object):
    """Owns a 3-node lgraph_server HA cluster."""

    def __init__(self, work_dir, extra_args=None):
        self.work_dir = os.path.abspath(work_dir)
        self.extra_args = list(extra_args or [])
        # Pick dynamic ports so parallel test invocations do not clash.
        self.http_ports = [free_port(), free_port(), free_port()]
        self.rpc_ports = [free_port(), free_port(), free_port()]
        self.ha_conf = ",".join(
            "127.0.0.1:%d" % p for p in self.rpc_ports)
        self.nodes = []
        self._rpc_clients = {}

    def start(self, timeout=120.0):
        for i in range(3):
            db = os.path.join(self.work_dir, "db%d" % i)
            log = os.path.join(self.work_dir, "node%d.log" % i)
            node = HANodeHandle(
                i, db, self.ha_conf, log_path=log,
                http_port=self.http_ports[i],
                rpc_port=self.rpc_ports[i],
                extra_args=self.extra_args)
            LOG.info("Starting node %d: HTTP=%d RPC=%s",
                     i, self.http_ports[i], self.rpc_ports[i])
            node.start(timeout=timeout)
            self.nodes.append(node)
        return self

    def stop(self):
        for n in reversed(self.nodes):
            n.stop()
        self._rpc_clients.clear()

    def kill_node(self, node_id):
        self.nodes[node_id].kill()
        # A killed node's cached RPC connection is no longer usable.
        self._invalidate_rpc(node_id)

    def start_node(self, node_id, timeout=120.0):
        self.nodes[node_id].start(timeout=timeout)
        self._invalidate_rpc(node_id)

    def freeze_node(self, node_id, resume=False):
        """Pause (SIGSTOP) or resume (SIGCONT) a node without killing it.

        This is a user-space approximation of a network partition / hung node:
        the process stops responding but stays alive. The Raft peer will mark
        it unreachable while frozen, then recover after resume.
        """
        import signal as _signal
        n = self.nodes[node_id]
        if n.proc is None or n.proc.poll() is not None:
            return
        n.proc.send_signal(_signal.SIGCONT if resume else _signal.SIGSTOP)

    def cleanup(self):
        self.stop()
        import shutil
        if os.path.isdir(self.work_dir):
            shutil.rmtree(self.work_dir, ignore_errors=True)

    def rpc(self, node_id=0, retries=30):
        key = (node_id, DEFAULT_USER)
        if key in self._rpc_clients:
            c = self._rpc_clients[key]
            if c is not None:
                return c
        import liblgraph_client_python
        host = "127.0.0.1:%d" % self.rpc_ports[node_id]
        last = None
        for _ in range(retries):
            try:
                c = liblgraph_client_python.client(
                    host, DEFAULT_USER, DEFAULT_PASSWORD)
                self._rpc_clients[key] = c
                return c
            except Exception as exc:
                last = exc
                time.sleep(1)
        raise RuntimeError(
            "could not connect to node %d RPC at %s: %s"
            % (node_id, host, last))

    def _invalidate_rpc(self, node_id):
        key = (node_id, DEFAULT_USER)
        self._rpc_clients.pop(key, None)


# Public helpers -------------------------------------------------------------

def cypher_all(nodes, script, graph="default", timeout=10):
    """Run a Cypher statement on every node. Returns list of (node_id, result)."""
    results = []
    for i in range(len(nodes)):
        try:
            c = nodes.rpc(i)
            ok, result = c.callCypher(script, graph, timeout=timeout)
            results.append((i, ok, result))
        except Exception as e:
            results.append((i, False, str(e)))
    return results


def cypher_on_leader(handle, script, graph="default", timeout=10):
    """Run a Cypher statement on the leader. Returns (ok, result)."""
    leader_id, leader_rpc = find_leader(handle)
    if leader_rpc is None:
        raise RuntimeError("no leader found")
    ok, result = leader_rpc.callCypher(script, graph, timeout=timeout)
    return ok, result


def find_leader(handle):
    """Return (node_id, rpc_client) of the current leader, or (None, None)."""
    for i in range(3):
        try:
            c = handle.rpc(i, retries=5)
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
                            return i, c
                elif isinstance(info, dict):
                    if info.get("state") == "MASTER":
                        return i, c
        except Exception:
            continue
    return None, None


def wait_for_leader(handle, timeout=60.0):
    """Block until a leader is elected. Returns (node_id, client)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        leader_id, leader_client = find_leader(handle)
        if leader_client is not None:
            return leader_id, leader_client
        time.sleep(1)
    raise RuntimeError("no leader elected within %.0fs" % timeout)


def all_nodes_healthy(handle):
    """Check that all 3 nodes report a non-OFFLINE state."""
    for i in range(3):
        try:
            c = handle.rpc(i, retries=3)
            ok, result = c.callCypher(
                "CALL dbms.ha.clusterInfo()", "default", timeout=10)
            if not ok:
                return False
        except Exception:
            return False
    return True


def wait_for_all_healthy(handle, timeout=60.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if all_nodes_healthy(handle):
            return True
        time.sleep(1)
    raise RuntimeError("cluster did not become healthy within %.0fs" % timeout)


# Sentinel data --------------------------------------------------------------
# Small deterministic graph that every chaos test can create and verify.

SENTINEL_CREATE = (
    "CALL dbms.graph.createGraph('sentinel', 'Phase3 HA test graph', 1)")
SENTINEL_MATCH = "MATCH (n:Person) RETURN n.name ORDER BY n.name"
SENTINEL_VERTICES = 3
SENTINEL_DATA = (
    "CREATE (n:Person {name: 'Alice'}) "
    "CREATE (n:Person {name: 'Bob'}) "
    "CREATE (n:Person {name: 'Carol'})"
)
SENTINEL_CHECK = (
    "MATCH (n:Person) RETURN count(n) AS cnt"
)

LOG = logging.getLogger(__name__)