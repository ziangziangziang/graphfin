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


def server_config():
    """Locate the HA server config shipped next to lgraph_server.

    lgraph_server requires -c <config>; it defaults to
    /usr/local/etc/lgraph.json which is not present in the test tree.
    """
    server_dir = os.path.dirname(server_binary())
    for candidate in (
        os.path.join(server_dir, "lgraph_ha.json"),
        os.path.join(BUILD_OUTPUT, "lgraph_ha.json"),
        "./lgraph_ha.json",
    ):
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)
    raise RuntimeError("lgraph_ha.json not found next to lgraph_server.")


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
        # First start bootstraps the group (role 1 for the seed, role 2 for
        # joiners). Every later start rejoins the existing group in
        # config-based mode (role 0) using --ha_conf.
        self.started_once = False

    def cmd(self):
        c = [
            server_binary(),
            "-c", server_config(),
            "--host", "127.0.0.1",
            "--port", str(self.http_port),
            "--enable_rpc", "true",
            "--rpc_port", str(self.rpc_port),
            "--directory", self.db_dir,
            # No --log_dir: the readiness marker ("Server started.") is an INFO
            # line on stderr/stdout, which start() captures. Setting --log_dir
            # would divert it into a rotating file and the readiness wait would
            # never see it.
            "--enable_ha", "true",
            "--ha_conf", self.ha_conf,
            # Keep peers in the Raft configuration across short chaos outages.
            # The aggressive test defaults (5s/10s) permanently remove a node
            # from the group after a brief kill; on restart it can then only
            # rejoin via add_peer, which makes repeated chaos runs degrade the
            # cluster. These wide values keep the group a stable 3 peers.
            "--ha_node_offline_ms", "600000",
            "--ha_node_remove_ms", "1200000",
            "--ha_node_join_group_s", "30",
            # The default 500ms election timeout is too tight for a busy
            # 2-core test host and causes leadership flapping. Give elections
            # room while keeping failover prompt.
            "--ha_election_timeout_ms", "3000",
            "--verbose", "1",
        ]
        if self.started_once:
            # Restart: rejoin the existing group from --ha_conf. Reusing the
            # bootstrap roles here would re-initialise the group and corrupt
            # it, so restarts always use role 0.
            c.extend(["--ha_bootstrap_role", "0"])
        elif self.node_id == 0:
            c.extend(["--ha_bootstrap_role", "1"])
        else:
            c.extend(["--ha_bootstrap_role", "2"])
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
                self.started_once = True
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
        # Nodes paused with SIGSTOP. A stopped process still accepts TCP
        # connections but never replies, and the python RPC binding does not
        # always honour its timeout during connect/login, so calls to a frozen
        # node can hang forever. Track them and skip in rpc().
        self._frozen = set()

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
        if resume:
            self._frozen.discard(node_id)
        else:
            self._frozen.add(node_id)
        # The peer's TCP connection stalls while stopped; drop the cached
        # client so the next rpc() reconnects cleanly after resume.
        self._invalidate_rpc(node_id)

    def cleanup(self):
        self.stop()
        if os.environ.get("HA_KEEP"):
            LOG.info("HA_KEEP set: leaving %s for inspection", self.work_dir)
            return
        import shutil
        if os.path.isdir(self.work_dir):
            shutil.rmtree(self.work_dir, ignore_errors=True)

    def rpc(self, node_id=0, retries=30):
        """Return a live, freshly-created client for a node.

        The python binding keeps process-global connection state, so a client
        cached across a node restart (or across a different node) can silently
        point at a dead socket. Always create a new client and validate it with
        a local `RETURN 1` (no master contact).
        """
        import liblgraph_client_python
        if node_id in self._frozen:
            raise RuntimeError("node %d is frozen (SIGSTOP); skipping" % node_id)
        host = "127.0.0.1:%d" % self.rpc_ports[node_id]
        last = None
        for _ in range(retries):
            try:
                c = liblgraph_client_python.client(
                    host, DEFAULT_USER, DEFAULT_PASSWORD)
                ok, _ = c.callCypher("RETURN 1 AS ok", "default", timeout=5)
                if ok:
                    return c
                last = "client created but not usable"
            except Exception as exc:
                last = exc
            time.sleep(1)
        raise RuntimeError(
            "could not connect to node %d RPC at %s: %s"
            % (node_id, host, last))

    def _invalidate_rpc(self, node_id):
        self._rpc_clients.pop(node_id, None)


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


def live_client(handle, prefer=None, retries=1):
    """Return a client to a node that answers, or raise.

    TuGraph's RPC server redirects write requests from a follower to the
    current leader, so a client to *any* live node can perform writes. This is
    far more robust than discovering the leader explicitly: it keeps working
    across elections as long as one node is up.
    """
    order = list(range(3))
    if prefer is not None:
        order = [prefer] + [i for i in order if i != prefer]
    last = None
    for i in order:
        try:
            return handle.rpc(i, retries=retries)
        except Exception as exc:  # noqa: BLE001
            last = exc
    raise RuntimeError("no live node: %s" % last)


def cypher_on_leader(handle, script, graph="default", timeout=15,
                     leader_timeout=90.0):
    """Run a Cypher statement, retrying against live nodes. Returns (ok, result).

    Writes executed on a follower are redirected by the server to the leader,
    so this works during elections and node restarts without explicit leader
    tracking.
    """
    deadline = time.time() + leader_timeout
    last = None
    while time.time() < deadline:
        try:
            c = live_client(handle)
            ok, result = c.callCypher(script, graph, timeout=timeout)
            if ok:
                return ok, result
            last = result
        except Exception as exc:  # noqa: BLE001
            last = exc
        time.sleep(1)
    return False, last


def _peers_from_cluster_info(result):
    rows = json.loads(result)
    for row in rows:
        info = row.get("cluster_info", row)
        if isinstance(info, str):
            info = json.loads(info)
        if isinstance(info, list):
            return info
    return []


def find_leader(handle):
    """Return (node_id, rpc_client) of the current leader, or (None, None).

    The leader is the peer reported with state MASTER in clusterInfo's peer
    list; its rpc_address maps back to a node index.
    """
    for i in range(3):
        try:
            c = handle.rpc(i, retries=1)
            ok, result = c.callCypher(
                "CALL dbms.ha.clusterInfo()", "default", timeout=8)
            if not ok:
                continue
            for peer in _peers_from_cluster_info(result):
                if peer.get("state") == "MASTER":
                    addr = peer.get("rpc_address") or peer.get("rpc_addr") or ""
                    if ":" in addr:
                        port = int(addr.rsplit(":", 1)[1])
                        for j in range(3):
                            if handle.rpc_ports[j] == port:
                                try:
                                    return j, handle.rpc(j, retries=1)
                                except Exception:  # noqa: BLE001
                                    return j, c
                    return i, c
        except Exception:
            continue
    return None, None


def wait_for_leader(handle, timeout=90.0):
    """Block until a leader is reported. Returns (node_id, client)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        leader_id, leader_client = find_leader(handle)
        if leader_client is not None:
            return leader_id, leader_client
        time.sleep(1)
    raise RuntimeError("no leader elected within %.0fs" % timeout)


def all_nodes_healthy(handle):
    """Check that all 3 nodes are reachable and report a peer list."""
    for i in range(3):
        try:
            c = handle.rpc(i, retries=2)
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
# TuGraph requires vertex labels to be declared before CREATE/MERGE, and the
# schema procedure runs in the context of the target graph.

SENTINEL_GRAPH = "sentinel"
SENTINEL_CREATE = (
    "CALL dbms.graph.createGraph('%s', 'Phase3 HA test graph', 1)" % SENTINEL_GRAPH)
SENTINEL_SCHEMA = [
    "CALL db.createVertexLabel('Person', 'name', 'name', 'STRING', false)",
    "CALL db.createVertexLabel('Counter', 'id', 'id', 'INT64', false, "
    "'value', 'INT64', false)",
]
SENTINEL_VERTICES = 3
SENTINEL_DATA = (
    "CREATE (a:Person {name: 'Alice'}), "
    "(b:Person {name: 'Bob'}), "
    "(c:Person {name: 'Carol'})"
)
SENTINEL_CHECK = "MATCH (n:Person) RETURN count(n) AS cnt"


def setup_sentinel(cluster, timeout=20.0):
    """Idempotently create the sentinel graph, its labels and initial data.

    Failures from a previous run (graph/label already exists) are ignored, so
    the fixture can be module-scoped and re-run safely.
    """
    leader_id, client = wait_for_leader(cluster, timeout=timeout)
    statements = [("default", SENTINEL_CREATE)]
    statements += [("sentinel", stmt) for stmt in SENTINEL_SCHEMA]
    statements.append(("sentinel", SENTINEL_DATA))
    for graph, stmt in statements:
        try:
            ok, result = client.callCypher(stmt, graph, timeout=timeout)
            if not ok:
                LOG.info("sentinel setup (ignored): %s -> %s", stmt, result)
        except Exception as exc:  # noqa: BLE001
            LOG.info("sentinel setup (ignored): %s -> %s", stmt, exc)


LOG = logging.getLogger(__name__)