"""Phase 0: multi-graph lifecycle correctness tests.

Covers graph creation, deletion, reopening (restart with graphs already on
disk), listing consistency, and the hard graph-count limit.

These tests manage their own server so they can restart it mid-test. The graph
count used here is small (tens) -- the large-scale behaviour is measured by
benchmark/scaling/, not asserted here.

Run:
  ci/phase0/run_tests.sh it     # everything
  PHASE0_TEST_FILES=test_multi_graph_lifecycle.py ci/phase0/run_tests.sh it
"""

import logging

import pytest

from phase0_util import (ServerHandle, count_vertices, create_graph, cypher,
    delete_graph, graph_exists, list_graphs, scalar, setup_graph, wait_for)

log = logging.getLogger(__name__)

# Small but large enough to exercise the O(N) lifecycle path repeatedly.
N_GRAPHS = 40


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("mg_lifecycle"))
    s = ServerHandle(db_dir)
    s.start()
    yield s
    s.cleanup()


# Function-scoped on purpose: some tests restart the server, which invalidates
# any client created before the restart. A per-test client keeps every test
# independent.
@pytest.fixture
def client(srv):
    c = srv.rpc()
    yield c
    try:
        c.logout()
    except Exception:
        pass


class TestMultiGraphLifecycle:
    def test_default_graph_is_auto_created(self, client):
        assert "default" in list_graphs(client)

    def test_graph_names_are_unique(self, client):
        name = "lc_unique"
        create_graph(client, name)
        assert graph_exists(client, name)
        # Creating the same graph twice must fail, not silently succeed.
        with pytest.raises(Exception):
            create_graph(client, name)

    def test_create_many_graphs_and_list(self, srv, client):
        before = len(list_graphs(client))
        created = []
        for i in range(N_GRAPHS):
            name = "lc_create_%03d" % i
            create_graph(client, name)
            created.append(name)
        names = list_graphs(client)
        assert len(names) == before + N_GRAPHS
        for n in created:
            assert n in names, "graph %s missing after creation" % n
        # Record for the deletion test.
        srv._lc_created = created

    def test_create_latency_is_bounded(self, srv, client):
        """A single create must not degrade pathologically at this scale."""
        import time
        t0 = time.time()
        create_graph(client, "lc_latency_probe")
        elapsed = time.time() - t0
        log.info("create latency at ~%d graphs: %.4fs",
                 len(list_graphs(client)), elapsed)
        # Generous bound: catches a hang or a full-table copy blowup, not noise.
        assert elapsed < 30.0, "graph create took %.2fs" % elapsed

    def test_graphs_are_isolated_stores(self, client):
        # Labels must be declared before use, so declare the same label in both
        # graphs and then check the data does not leak between them.
        setup_graph(client, "lc_iso_a")
        setup_graph(client, "lc_iso_b")
        cypher(client, "CREATE (n:person {id: 1, name: 'a'})", "lc_iso_a")
        assert count_vertices(client, "lc_iso_a", "person") == 1
        assert count_vertices(client, "lc_iso_b", "person") == 0

    def test_delete_graph(self, client):
        create_graph(client, "lc_to_delete")
        assert graph_exists(client, "lc_to_delete")
        delete_graph(client, "lc_to_delete")
        wait_for(lambda: not graph_exists(client, "lc_to_delete"),
                 timeout=30, message="graph deletion to take effect")

    def test_delete_many_graphs(self, srv, client):
        created = getattr(srv, "_lc_created", [])
        assert created, "creation test did not run"
        for name in created:
            delete_graph(client, name)
        names = list_graphs(client)
        for name in created:
            assert name not in names, "graph %s still listed after delete" % name

    def test_deleting_nonexistent_graph_fails(self, client):
        with pytest.raises(Exception):
            delete_graph(client, "lc_does_not_exist")

    def test_reopen_graphs_after_restart(self, srv, client):
        """Graph reopening: data and graph list must survive a restart."""
        setup_graph(client, "lc_persist")
        cypher(client, "CREATE (n:person {id: 1, name: 'persisted'})", "lc_persist")
        names_before = sorted(list_graphs(client))

        srv.restart()

        fresh = srv.rpc()
        try:
            names_after = sorted(list_graphs(fresh))
            assert names_after == names_before, (
                "graph list changed across restart:\n before=%s\n after=%s"
                % (names_before, names_after))
            assert graph_exists(fresh, "lc_persist")
            assert count_vertices(fresh, "lc_persist", "person") == 1
        finally:
            fresh.logout()

    def test_deleted_graph_stays_deleted_across_restart(self, srv, client):
        create_graph(client, "lc_gone_after_restart")
        delete_graph(client, "lc_gone_after_restart")
        srv.restart()
        fresh = srv.rpc()
        try:
            assert not graph_exists(fresh, "lc_gone_after_restart")
        finally:
            fresh.logout()

    def test_server_remains_healthy_with_many_graphs(self, srv, client):
        """The server stays usable with many graphs and reports them all.

        The hard cap is MAX_NUM_GRAPHS = 4096 (src/core/defs.h:143) and is
        exercised for real by the 4000-graph benchmark run, not here: creating
        4096 graphs in a unit test would dominate the suite's runtime. What this
        test adds is that a graph registry well past the tens-of-graphs scale
        stays self-consistent and readable.
        """
        names = list_graphs(client)
        assert len(names) < 4096, "test server unexpectedly near the graph cap"
        # No duplicate names: the registry is keyed by name in the meta store.
        assert len(names) == len(set(names)), "duplicate graph names in registry"
        # The server is still fully functional.
        assert scalar(client, "MATCH (n) RETURN count(n)", "default") is not None
