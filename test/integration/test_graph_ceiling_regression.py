"""Regression test for R0: the ~998-graph ceiling must be gone.

Background
----------
Every graph is its own LMDB environment. Before the fix, TuGraph opened LMDB
without `MDB_NOTLS`, so LMDB allocated one pthread thread-specific-data key per
environment (`src/core/lmdb/mdb.c:5211`). glibc caps process-wide TLS keys at
`PTHREAD_KEYS_MAX = 1024` -- a compile-time constant, not an adjustable rlimit --
so graph creation failed deterministically at graph **#998** with
`Resource temporarily unavailable` (EAGAIN).

The fix sets `MDB_NOTLS` (configurable as `lmdb_notls`, default true). These
tests assert the ceiling is gone, on both the create path and the reopen path,
and that the policy is actually in effect.

Run:
  PHASE0_TEST_FILES=test_graph_ceiling_regression.py dev/phase0/run_tests.sh it
  PHASE0_CEILING_N=2000 PHASE0_TEST_FILES=test_graph_ceiling_regression.py \
      dev/phase0/run_tests.sh it
"""

import logging
import os

import pytest

from phase0_util import (
    ServerHandle, count_vertices, create_graph, cypher, delete_graph,
    graph_exists, list_graphs,
)

log = logging.getLogger(__name__)

# Comfortably past the old 998 ceiling. Override with PHASE0_CEILING_N.
CEILING_N = int(os.environ.get("PHASE0_CEILING_N", "1200"))

VERTEX_SCHEMA = (
    "CALL db.createVertexLabel('person', 'id', "
    "'id', 'INT64', false, 'name', 'string', true)"
)


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("r0_ceiling"))
    s = ServerHandle(db_dir)
    s.start()
    yield s
    s.cleanup()


@pytest.fixture
def client(srv):
    c = srv.rpc()
    yield c
    try:
        c.logout()
    except Exception:
        pass


def _config_map(client):
    res = cypher(client, "CALL dbms.config.list()")
    cfg = {}
    for row in (res or []):
        if isinstance(row, dict) and "name" in row:
            cfg[row["name"]] = row["value"]
    return cfg


class TestGraphCeilingRegression:
    def test_lmdb_notls_policy_is_enabled(self, client):
        """The setting must be registered, default to true, and be visible."""
        cfg = _config_map(client)
        assert "lmdb_notls" in cfg, (
            "lmdb_notls missing from dbms.config.list(); registered keys: %s"
            % sorted(cfg)[:40])
        assert str(cfg["lmdb_notls"]).strip().lower() in ("true", "1"), (
            "lmdb_notls must default to true, got %r" % (cfg["lmdb_notls"],))

    def test_can_create_graphs_past_the_old_ceiling(self, client):
        """This is the regression: creation used to fail at graph #998."""
        before = len(list_graphs(client))
        names = ["r0g%05d" % i for i in range(CEILING_N)]
        for n in names:
            create_graph(client, n)

        after = list_graphs(client)
        assert len(after) == before + len(names), (
            "expected %d graphs, found %d" % (before + len(names), len(after)))
        missing = [n for n in names if n not in after]
        assert not missing, (
            "creation did not persist for: %s%s" %
            (missing[:5], " (+%d more)" % (len(missing) - 5) if len(missing) > 5 else ""))
        log.info("opened %d graphs; the old ceiling was 998", len(after))

    def test_graph_created_past_the_old_ceiling_is_usable(self, client):
        """A graph beyond #998 must be a fully functional graph, not just listed."""
        g = "r0g%05d" % (CEILING_N - 1)
        assert g in list_graphs(client)
        cypher(client, VERTEX_SCHEMA, g)
        cypher(client, "CREATE (n:person {id: 1, name: 'beyond'})", g)
        assert count_vertices(client, g, "person") == 1

    def test_reopen_more_than_998_graphs_on_restart(self, srv, client):
        """The open path hit the same limit: reopening ~1000 graphs also failed.

        After the Phase 2 lazy-loading change a restart only loads the catalog;
        graphs are re-opened lazily on first access. This still confirms the
        registered count survives a restart. The reopen path under load is
        exercised at larger scale by test_graph_lifecycle_restart.py.
        """
        before = len(list_graphs(client))
        assert before > 998, "expected more than 998 graphs, found %d" % before

        srv.restart()

        fresh = srv.rpc()
        try:
            after = list_graphs(fresh)
            assert len(after) == before, (
                "graph count changed across restart: %d -> %d" % (before, len(after)))
        finally:
            fresh.logout()


class TestConfiguredGraphLimit:
    """`max_graphs` replaces the former hard-coded 4096.

    With a small configured limit the boundary is cheap to exercise exactly,
    which also covers the off-by-one between the two creation paths
    (CreateGraph checked `size()`, CreateGraphWithData checked `size() + 1`,
    so they disagreed by one graph).
    """

    # The auto-created "default" graph counts toward the limit, so LIMIT=3
    # allows exactly 2 extra graphs. LIMIT is small on purpose: with it set
    # low, both creation paths are exercised at the exact boundary.
    LIMIT = 3

    @pytest.fixture(scope="class")
    def limited_srv(self, tmp_path_factory):
        db_dir = str(tmp_path_factory.mktemp("r0_limit"))
        s = ServerHandle(db_dir, extra_args=["--max_graphs", str(self.LIMIT)])
        s.start()
        yield s
        s.cleanup()

    @pytest.fixture
    def limited_client(self, limited_srv):
        c = limited_srv.rpc()
        yield c
        try:
            c.logout()
        except Exception:
            pass

    def test_creating_up_to_the_limit_succeeds(self, limited_client):
        for i in range(self.LIMIT - 1):
            create_graph(limited_client, "lim_%02d" % i)
        assert len(list_graphs(limited_client)) == self.LIMIT

    def test_exceeding_the_limit_is_rejected(self, limited_client):
        with pytest.raises(Exception):
            create_graph(limited_client, "lim_over")

    def test_deleting_frees_a_slot(self, limited_client):
        delete_graph(limited_client, "lim_00")
        create_graph(limited_client, "lim_reuse")
        assert graph_exists(limited_client, "lim_reuse")

    def test_limit_is_visible_in_config(self, limited_client):
        res = cypher(limited_client, "CALL dbms.config.list()")
        cfg = {row["name"]: row["value"]
               for row in (res or []) if isinstance(row, dict) and "name" in row}
        assert str(cfg.get("max_graphs")) == str(self.LIMIT), (
            "max_graphs not reflected in dbms.config.list(): %r" % (cfg.get("max_graphs"),))
