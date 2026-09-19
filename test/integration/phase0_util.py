"""Shared helpers for the Phase 0 multi-graph integration tests.

These tests are deliberately self-contained: they launch and control their own
`lgraph_server` processes so they can exercise restart, SIGKILL and multi-graph
scaling without fighting the function-scoped fixtures in conftest.py.

There is intentional overlap with benchmark/scaling/{serverctl,restclient}.py.
The test tree is copied into build/output by the CI flow (see
ci/github_ci.sh and ci/phase0/run_tests_inner.sh), so keeping these helpers
dependency-free and local keeps the tests hermetic.

Client API facts these helpers rely on (verified against TuGraph 4.5.2):
  * RPC python binding: liblgraph_client_python.client("host:port", user, pass)
    with .callCypher(script, graph) -> (ok: bool, result_json: str)
  * Cypher graph procedures:
      dbms.graph.createGraph(name, description, max_size_GB)
      dbms.graph.deleteGraph(name)
      dbms.graph.listGraphs()
      dbms.graph.getGraphInfo(name)
  * Server readiness marker: the "Server started." log line
    (src/server/lgraph_server.cpp:379)
"""

import json
import os
import shutil
import signal
import socket
import subprocess
import time

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILD_OUTPUT = os.path.join(REPO_ROOT, "build", "output")

DEFAULT_USER = "admin"
DEFAULT_PASSWORD = "73@TuGraph"
READY_MARKER = "Server started."

# Graph creation requires an explicit max_size_GB over REST, and the engine
# default is 4 TiB (src/core/defs.h:154). Always pass a small explicit size so
# multi-graph tests cannot exhaust virtual address space.
GRAPH_SIZE_GB = 1


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def server_binary():
    for candidate in (os.path.join(BUILD_OUTPUT, "lgraph_server"), "./lgraph_server"):
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)
    raise RuntimeError(
        "lgraph_server not found. Build it with ci/phase0/build.sh first.")


def server_config():
    for candidate in (os.path.join(BUILD_OUTPUT, "lgraph_standalone.json"),
                      "./lgraph_standalone.json"):
        if os.path.isfile(candidate):
            return os.path.abspath(candidate)
    raise RuntimeError(
        "lgraph_standalone.json not found next to lgraph_server.")


class ServerHandle(object):
    """Owns one lgraph_server process and its data directory."""

    def __init__(self, db_dir, log_path=None, extra_args=None, enable_rpc=True):
        self.db_dir = os.path.abspath(db_dir)
        self.log_path = log_path or os.path.join(self.db_dir + ".log")
        self.extra_args = list(extra_args or [])
        self.enable_rpc = enable_rpc
        self.proc = None
        self._logf = None
        self.http_port = None
        self.rpc_port = None
        self.bolt_port = None
        self.start_seconds = None

    def cmd(self):
        c = [server_binary(), "-c", server_config(),
             "--directory", self.db_dir,
             "--host", "127.0.0.1",
             "--port", str(self.http_port),
             "--enable_rpc", "true" if self.enable_rpc else "false",
             "--rpc_port", str(self.rpc_port),
             "--bolt_port", str(self.bolt_port)]
        return c + self.extra_args

    def start(self, timeout=300.0):
        if self.proc is not None and self.proc.poll() is None:
            raise RuntimeError("server already running")
        os.makedirs(self.db_dir, exist_ok=True)
        if self.http_port is None:
            # Preserve the same ports across a restart so clients can reconnect.
            self.http_port = free_port()
            self.rpc_port = free_port()
            self.bolt_port = free_port()
        self._logf = open(self.log_path, "w")
        t0 = time.time()
        self.proc = subprocess.Popen(
            self.cmd(), stdout=self._logf, stderr=subprocess.STDOUT,
            close_fds=True, cwd=os.path.dirname(server_binary()))
        deadline = t0 + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("server exited early:\n%s" % self.log_tail())
            if self._log_has(READY_MARKER):
                self.start_seconds = time.time() - t0
                return self
            time.sleep(0.05)
        self.kill()
        raise RuntimeError("server did not become ready in %.0fs:\n%s"
                           % (timeout, self.log_tail()))

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

    def stop(self, timeout=180.0):
        if not self.alive():
            return None
        t0 = time.time()
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.kill()
            raise RuntimeError("server ignored SIGTERM for %.0fs" % timeout)
        if self._logf:
            self._logf.close()
            self._logf = None
        return time.time() - t0

    def kill(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                pass
        if self._logf:
            self._logf.close()
            self._logf = None

    def restart(self, timeout=300.0):
        self.stop()
        return self.start(timeout=timeout)

    def cleanup(self):
        self.kill()
        for d in (self.db_dir,):
            if os.path.isdir(d):
                shutil.rmtree(d, ignore_errors=True)
        if os.path.isfile(self.log_path):
            try:
                os.remove(self.log_path)
            except OSError:
                pass

    # -- clients ------------------------------------------------------------

    def rpc(self, retries=60, interval=1.0):
        """Connect to the RPC port, retrying while the server comes up."""
        import liblgraph_client_python
        host = "127.0.0.1:%d" % self.rpc_port
        last = None
        for _ in range(retries):
            try:
                return liblgraph_client_python.client(
                    host, DEFAULT_USER, DEFAULT_PASSWORD)
            except Exception as exc:  # noqa: BLE001
                last = exc
                time.sleep(interval)
        raise RuntimeError("could not connect to RPC at %s: %s" % (host, last))


# ---------------------------------------------------------------------------
# Cypher helpers
# ---------------------------------------------------------------------------

class CypherError(RuntimeError):
    pass


def cypher(client, script, graph="default"):
    """Run Cypher, return parsed JSON result. Raises on failure."""
    ok, result = client.callCypher(script, graph)
    if not ok:
        raise CypherError("Cypher failed on graph %r: %s\n  script: %s"
                          % (graph, result, script))
    try:
        return json.loads(result)
    except (ValueError, TypeError):
        return result


def cypher_ok(client, script, graph="default"):
    """Run Cypher and return only the success flag.

    Use this for authorization tests: the RPC binding reports an access denial
    as (False, message) rather than raising.
    """
    ok, _ = client.callCypher(script, graph)
    return bool(ok)


def create_graph(client, name, size_gb=GRAPH_SIZE_GB, desc="phase0 test graph"):
    return cypher(client, "CALL dbms.graph.createGraph('%s', '%s', %d)"
                  % (name, desc, size_gb))


def create_graph_if_missing(client, name, size_gb=GRAPH_SIZE_GB):
    if name not in list_graphs(client):
        return create_graph(client, name, size_gb)
    return None


def delete_graph(client, name):
    return cypher(client, "CALL dbms.graph.deleteGraph('%s')" % name)


# TuGraph requires a vertex label to be declared before vertices of that label
# can be created; `CREATE (n:undeclared {...})` fails with
# "No such vertex label". Tests therefore declare a simple schema first.
VERTEX_LABEL_TEMPLATE = (
    "CALL db.createVertexLabel('%s', '%s', '%s', '%s', false, 'name', 'string', true)"
)


def declare_vertex_label(client, graph, label="person",
                         primary="id", primary_type="INT64"):
    """Declare a two-field vertex label. Idempotent."""
    stmt = VERTEX_LABEL_TEMPLATE % (label, primary, primary, primary_type)
    try:
        cypher(client, stmt, graph)
    except CypherError:
        # Already declared, which is fine for an idempotent setup helper.
        pass


def setup_graph(client, graph, label="person"):
    """Create a graph if needed and declare the standard test schema."""
    create_graph_if_missing(client, graph)
    declare_vertex_label(client, graph, label)
    return graph


def list_graphs(client):
    """Return the list of graph names."""
    res = cypher(client, "CALL dbms.graph.listGraphs()")
    names = []
    if isinstance(res, list):
        for row in res:
            if isinstance(row, dict):
                if "graph_name" in row:
                    names.append(row["graph_name"])
                elif "name" in row:
                    names.append(row["name"])
            elif isinstance(row, list) and row:
                names.append(row[0])
    return names


def graph_exists(client, name):
    return name in list_graphs(client)


def first_value(res):
    """Extract the first cell of the first row from a Cypher result.

    The RPC binding returns a JSON array of row objects (or arrays), e.g.
    `[{"count(n)": 3}]` or `[[3]]`.
    """
    if isinstance(res, list) and res:
        row = res[0]
        if isinstance(row, dict):
            for v in row.values():
                return v
        if isinstance(row, list) and row:
            return row[0]
    return None


def scalar(client, script, graph="default"):
    return first_value(cypher(client, script, graph))


def count_vertices(client, graph, label=None):
    """Count vertices, working around an engine limitation on empty labels.

    TuGraph 4.5.2 rejects a label-filtered count when the label currently has no
    vertices:

        MATCH (n:person) RETURN count(n)
        -> [InputError] capacity cannot be 0

    That is a problem precisely for multi-graph workloads, where many graphs are
    created empty. This helper asks for the count first and falls back to
    counting returned rows, which does not hit the faulty path.
    """
    if label is None:
        return scalar(client, "MATCH (n) RETURN count(n)", graph)
    try:
        return scalar(client, "MATCH (n:%s) RETURN count(n)" % label, graph)
    except CypherError:
        rows = cypher(client, "MATCH (n:%s) RETURN n" % label, graph)
        return len(rows) if isinstance(rows, list) else 0


def wait_for(predicate, timeout=60.0, interval=1.0, message="condition"):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    raise AssertionError("timed out waiting for %s" % message)
