"""Phase 0: snapshot and restore correctness with multiple graphs.

Exercises the only whole-server backup path available in-process:
`CALL dbms.takeSnapshot()` -> `Galaxy::SaveSnapshot`
(src/cypher/procedure/procedure.cpp:2349-2365, src/db/galaxy.cpp:505-531),
which iterates and copies every graph sequentially
(src/db/graph_manager.cpp:298-316).

Restore has no dedicated tool: it is a directory copy plus a server start
(src/db/galaxy.cpp:481-496 is the in-process variant used by braft). This test
performs exactly that procedure and verifies the restored database.

Run:
  PHASE0_TEST_FILES=test_backup_restore_scale.py ci/phase0/run_tests.sh it
"""

import logging
import os
import shutil

import pytest

from phase0_util import (ServerHandle, count_vertices, create_graph, cypher,
    graph_exists, list_graphs, scalar, wait_for)

log = logging.getLogger(__name__)

VERTEX_SCHEMA = (
    "CALL db.createVertexLabel('person', 'id', "
    "'id', 'INT64', false, 'name', 'string', true)"
)

# Enough graphs that the O(N) serial copy in GraphManager::Backup is exercised
# without making the test slow.
N_GRAPHS = 12
ROWS_PER_GRAPH = 5


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("mg_backup"))
    s = ServerHandle(db_dir)
    s.start()
    yield s
    s.cleanup()


# Function-scoped: this suite stops and restarts the source server.
@pytest.fixture
def client(srv):
    c = srv.rpc()
    yield c
    try:
        c.logout()
    except Exception:
        pass


def populate(client, n_graphs=N_GRAPHS, rows=ROWS_PER_GRAPH):
    populated = {}
    for i in range(n_graphs):
        g = "bk_graph_%02d" % i
        create_graph(client, g)
        cypher(client, VERTEX_SCHEMA, g)
        for r in range(rows):
            cypher(client, "CREATE (n:person {id: %d, name: 'g%dr%d'})" % (r, i, r), g)
        populated[g] = rows
    return populated


class TestBackupRestoreScale:
    def test_snapshot_creates_a_complete_tree(self, srv, client):
        populated = populate(client)
        srv._bk_populated = populated

        snap_root = os.path.join(srv.db_dir, "snapshot")
        cypher(client, "CALL dbms.takeSnapshot()", "default")

        wait_for(lambda: os.path.isdir(snap_root) and len(os.listdir(snap_root)) > 0,
                 timeout=300, message="snapshot directory to appear")
        latest = sorted(os.listdir(snap_root))[-1]
        snap = os.path.join(snap_root, latest)

        # The snapshot must contain the meta store and one directory per graph.
        assert os.path.isdir(os.path.join(snap, ".meta")), \
            "snapshot is missing the .meta store"
        assert os.path.isfile(os.path.join(snap, ".meta", "data.mdb")), \
            "snapshot .meta has no data.mdb"

        # Count graph dirs, excluding .meta.
        graph_dirs = [d for d in os.listdir(snap)
                      if os.path.isdir(os.path.join(snap, d)) and d != ".meta"]
        expected = len(populated) + 1        # + the auto-created 'default' graph
        assert len(graph_dirs) >= expected, (
            "snapshot has %d graph dirs, expected at least %d"
            % (len(graph_dirs), expected))

    def test_restore_snapshot_into_fresh_dir(self, srv, client, tmp_path_factory):
        snap_root = os.path.join(srv.db_dir, "snapshot")
        latest = sorted(os.listdir(snap_root))[-1]
        snap = os.path.join(snap_root, latest)
        populated = getattr(srv, "_bk_populated", {})

        restore_dir = str(tmp_path_factory.mktemp("mg_restore"))
        # copytree requires the destination not to exist.
        shutil.rmtree(restore_dir)
        shutil.copytree(snap, restore_dir)

        # Stop the source server first: restore is a cold path and both would
        # otherwise compete for the same ports and files.
        srv.stop()

        restored = ServerHandle(restore_dir)
        try:
            restored.start()
            c = restored.rpc()
            try:
                names = list_graphs(c)
                for g, rows in populated.items():
                    assert g in names, "graph %s missing after restore" % g
                    assert graph_exists(c, g)
                    assert count_vertices(c, g, "person") == rows, \
                        "row count mismatch in restored graph %s" % g
                assert "default" in names
            finally:
                c.logout()
        finally:
            restored.stop()
            shutil.rmtree(restore_dir, ignore_errors=True)

        # Leave the module's server running for teardown symmetry.
        srv.start()

    def test_source_db_is_still_usable_after_snapshot(self, srv, client):
        """A snapshot must not disturb the running database."""
        populated = getattr(srv, "_bk_populated", {})
        assert populated, "populate step did not run"
        c = srv.rpc()
        try:
            for g, rows in list(populated.items())[:3]:
                assert count_vertices(c, g, "person") == rows
        finally:
            c.logout()
