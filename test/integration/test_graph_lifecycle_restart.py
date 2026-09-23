"""Phase 2 closeout: restart with a large registered-graph population.

Success criteria (PROJECT.md:320, 327, 328):

  * 320 -- server startup must not physically open every graph;
  * 327 -- a restart with a large registered population must succeed and the
    full graph list must survive;
  * 328 -- randomly selected graphs must remain readable and writable after
    the restart.

The full 100k target is exercised end-to-end by benchmark/scaling
(phase2-100k.json). Here the population is env-driven (PHASE2_RESTART_N,
default 2000) so the assertion runs inside CI without dominating its runtime,
while still being far past the old 998 ceiling and large enough that an
"open every graph on startup" regression would be obvious (the default
max_open_graphs is 1000, so a non-lazy restart of 2000 graphs would blow the
limit and open far more than a handful).

Run:
  PHASE2_RESTART_N=2000 \
      PHASE0_TEST_FILES=test_graph_lifecycle_restart.py dev/phase0/run_tests.sh it
  # full criterion scale (slow; mirrors the benchmark):
  PHASE2_RESTART_N=100000 \
      PHASE0_TEST_FILES=test_graph_lifecycle_restart.py dev/phase0/run_tests.sh it
"""

import logging
import os

import pytest

from phase0_util import (
    ServerHandle, count_vertices, create_graph, cypher, declare_vertex_label,
    list_graphs,
)

log = logging.getLogger(__name__)

N = int(os.environ.get("PHASE2_RESTART_N", "2000"))
LABEL = "person"
# First / middle / last positions: "randomly selected" sentinels that must
# survive a restart intact.
SENTINEL_IDX = (0, N // 2, N - 1)
OPEN_GRAPH_BOUND = 16  # lazy startup must not open the whole population


def _cache_stats(c):
    res = cypher(c, "CALL dbms.graph.cacheStats()")
    if isinstance(res, list) and res:
        return res[0]
    return {}


def _sentinel_name(idx):
    return "p2rs_%05d" % idx


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("p2_restart"))
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


class TestLargePopulationRestart:
    def test_restart_keeps_all_graphs_and_stays_lazy(self, srv, client):
        # Build the population and write a unique marker into the sentinels.
        names = [_sentinel_name(i) for i in range(N)]
        for n in names:
            create_graph(client, n)
        listed = set(list_graphs(client))
        registered = len(listed & set(names))
        assert registered == N, (
            "expected %d graphs registered, got %d" % (N, registered))

        for idx in SENTINEL_IDX:
            g = _sentinel_name(idx)
            declare_vertex_label(client, g, LABEL)
            cypher(client,
                   "CREATE (n:%s {id: %d, name: 'sentinel'})" % (LABEL, idx),
                   g)
            assert count_vertices(client, g, LABEL) == 1, (
                "sentinel %s not written before restart" % g)

        # Restart and re-connect.
        srv.restart()
        fresh = srv.rpc()
        try:
            # 320: a restart must not open every graph. Sample immediately,
            # before any post-restart access, so the lazy path is exercised.
            stats = _cache_stats(fresh)
            open_now = int(stats.get("open_graphs", 0))
            assert open_now <= OPEN_GRAPH_BOUND, (
                "startup opened %d graphs (bound %d); lazy loading regressed"
                % (open_now, OPEN_GRAPH_BOUND))

            # 327: the full graph list survives the restart.
            after = set(list_graphs(fresh))
            assert len(after & set(names)) == N, (
                "graph count changed across restart: %d -> %d"
                % (N, len(after & set(names))))

            # 328: sentinels remain readable (marker present) and writable
            # (a fresh write sticks) after the restart.
            for idx in SENTINEL_IDX:
                g = _sentinel_name(idx)
                assert count_vertices(fresh, g, LABEL) == 1, (
                    "sentinel %s lost its data across restart" % g)
                # A distinct primary key (the marker used `id: idx`) so this
                # write proves writability without colliding with the marker.
                cypher(fresh,
                       "CREATE (n:%s {id: %d, name: 'post'})" % (LABEL, idx + 1000000),
                       g)
                assert count_vertices(fresh, g, LABEL) == 2, (
                    "sentinel %s not writable after restart" % g)
        finally:
            fresh.logout()
