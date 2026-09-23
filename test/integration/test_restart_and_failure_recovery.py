"""Phase 0: restart and crash recovery with multiple graphs.

Covers:
  * graceful restart with data written across several graphs
  * SIGKILL while idle, then restart
  * SIGKILL in the middle of a large single transaction, then restart

Snapshot-isolation expectation: the killed transaction must be either fully
present or fully absent, never partially applied.

Durability caveat (important, and asserted accordingly): LMDB environments are
opened with MDB_NOSYNC unconditionally (src/core/lmdb_store.cpp:60-65) and
`durable` defaults to false (src/core/data_type.h:156). A SIGKILL can therefore
lose recently acknowledged writes. These tests assert *consistency and
recoverability*, not durability of unflushed writes.

Run:
  PHASE0_TEST_FILES=test_restart_and_failure_recovery.py dev/phase0/run_tests.sh it
"""

import logging
import threading
import time

import pytest

from phase0_util import (ServerHandle, count_vertices, create_graph, cypher,
    graph_exists, list_graphs, scalar)

log = logging.getLogger(__name__)

VERTEX_SCHEMA = (
    "CALL db.createVertexLabel('person', 'id', "
    "'id', 'INT64', false, 'name', 'string', true)"
)
BIG_COUNT = 2000  # one comma-separated CREATE => one transaction


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("mg_recovery"))
    s = ServerHandle(db_dir)
    s.start()
    yield s
    s.cleanup()


# Function-scoped: this suite restarts and SIGKILLs the server, which
# invalidates any client created before the restart.
@pytest.fixture
def client(srv):
    c = srv.rpc()
    yield c
    try:
        c.logout()
    except Exception:
        pass


def fresh_client(srv):
    return srv.rpc()


class TestRestartRecovery:
    def test_graceful_restart_preserves_multi_graph_data(self, srv, client):
        graphs = {"rc_a": 5, "rc_b": 9, "rc_c": 1}
        for g, n in graphs.items():
            create_graph(client, g)
            cypher(client, VERTEX_SCHEMA, g)
            for i in range(n):
                cypher(client, "CREATE (n:person {id: %d, name: 'v%d'})" % (i, i), g)

        srv.restart()
        c = fresh_client(srv)
        try:
            for g, n in graphs.items():
                assert graph_exists(c, g), "graph %s lost across restart" % g
                assert count_vertices(c, g, "person") == n, \
                    "data loss in %s across graceful restart" % g
        finally:
            c.logout()

    def test_sigkill_while_idle_then_restart(self, srv, client):
        create_graph(client, "rc_kill_idle")
        cypher(client, VERTEX_SCHEMA, "rc_kill_idle")
        cypher(client, "CREATE (n:person {id: 1, name: 'before-kill'})",
               "rc_kill_idle")
        names_before = sorted(list_graphs(client))

        srv.kill()          # SIGKILL: no graceful shutdown at all
        assert not srv.alive()

        srv.start()         # must recover from whatever was on disk
        c = fresh_client(srv)
        try:
            names_after = sorted(list_graphs(c))
            assert names_after == names_before, (
                "graph registry inconsistent after SIGKILL:\n before=%s\n after=%s"
                % (names_before, names_after))
            # The graph must be openable and readable.
            assert count_vertices(c, "rc_kill_idle", "person") is not None
        finally:
            c.logout()

    def test_sigkill_mid_transaction_is_atomic(self, srv, client):
        """A large single transaction interrupted by SIGKILL must be all-or-nothing."""
        create_graph(client, "rc_atomic")
        cypher(client, VERTEX_SCHEMA, "rc_atomic")

        # One statement == one transaction. Built as a comma-separated CREATE
        # list rather than `UNWIND ... CREATE`, because in the default Cypher v2
        # engine `UNWIND ... CREATE` only creates ONE vertex no matter how many
        # rows the UNWIND produces (see docs/architecture/08-correctness-findings.md).
        patterns = ", ".join(
            "(p%d:person {id: %d, name: 'bulk'})" % (i, i)
            for i in range(1, BIG_COUNT + 1))
        big = "CREATE " + patterns

        result = {}

        def writer():
            try:
                c = srv.rpc(retries=5, interval=0.5)
                ok, msg = c.callCypher(big, "rc_atomic")
                result["ok"] = bool(ok)
                result["msg"] = str(msg)[:200]
            except Exception as exc:  # noqa: BLE001
                result["error"] = str(exc)

        t = threading.Thread(target=writer)
        t.start()
        # Let the transaction begin and write for a moment, then kill mid-flight.
        time.sleep(0.2)
        killed_alive = srv.alive()
        srv.kill()
        t.join(timeout=30)

        assert killed_alive, "server died before the kill, test is meaningless"

        srv.start()
        c = fresh_client(srv)
        try:
            # Unlabelled count: a label-filtered count fails on an empty label
            # with "capacity cannot be 0" (same findings document).
            count = count_vertices(c, "rc_atomic")
            log.info("after mid-transaction SIGKILL: count=%s writer=%s",
                     count, result)
            assert count in (0, BIG_COUNT), (
                "partial transaction applied: count=%s, expected 0 or %d"
                % (count, BIG_COUNT))
            # The graph must remain fully usable afterwards.
            cypher(c, "CREATE (n:person {id: %d, name: 'after'})" % (BIG_COUNT + 1),
                   "rc_atomic")
            assert count_vertices(c, "rc_atomic") == count + 1
        finally:
            c.logout()

    def test_repeated_kill_restart_cycles_do_not_corrupt(self, srv, client):
        create_graph(client, "rc_cycles")
        cypher(client, VERTEX_SCHEMA, "rc_cycles")
        for cycle in range(3):
            c = fresh_client(srv)
            try:
                cypher(c, "CREATE (n:person {id: %d, name: 'cycle%d'})"
                       % (cycle, cycle), "rc_cycles")
            finally:
                c.logout()
            srv.kill()
            srv.start()

        c = fresh_client(srv)
        try:
            assert graph_exists(c, "rc_cycles")
            # At least the writes that were flushed must be present and readable.
            assert count_vertices(c, "rc_cycles", "person") is not None
            # The server must still accept writes.
            cypher(c, "CREATE (n:person {id: 999, name: 'final'})", "rc_cycles")
        finally:
            c.logout()
