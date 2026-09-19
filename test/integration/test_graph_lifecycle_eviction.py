"""Phase 2 closeout: concurrent access while eviction is active.

Success criterion (PROJECT.md:326): "concurrent access and eviction tests
pass".

This test drives many threads that read/write randomly across a graph
population larger than `max_open_graphs`, forcing the LRU eviction policy to
run continuously while transactions are in flight. It asserts that:

  * no read or write errors occur -- eviction must never interrupt an active
    transaction (the ScopedRef / HasOutstandingRefs guard);
  * the cache actually evicts (evictions > 0);
  * the physically-open graph count stays bounded -- it never opens the whole
    population, and stays within `max_open_graphs` plus the in-flight threads.

The population, cache size, thread count and iteration count are env-driven so
the test can be tightened or loosened without code edits.

Run:
  PHASE2_EVICT_POP=200 PHASE2_EVICT_THREADS=8 \
      PHASE0_TEST_FILES=test_graph_lifecycle_eviction.py ci/phase0/run_tests.sh it
"""

import logging
import os
import random
import threading

import pytest

from phase0_util import (
    ServerHandle, count_vertices, create_graph, cypher, declare_vertex_label,
    list_graphs,
)

log = logging.getLogger(__name__)

MAX_OPEN = int(os.environ.get("PHASE2_EVICT_MAX_OPEN", "8"))
POPULATION = int(os.environ.get("PHASE2_EVICT_POP", "200"))
THREADS = int(os.environ.get("PHASE2_EVICT_THREADS", "8"))
ITERS = int(os.environ.get("PHASE2_EVICT_ITERS", "100"))
IDLE_TIMEOUT_S = os.environ.get("PHASE2_EVICT_IDLE_S", "1")

LABEL = "person"


def _cache_stats(c):
    res = cypher(c, "CALL dbms.graph.cacheStats()")
    if isinstance(res, list) and res:
        return res[0]
    return {}


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("p2_evict"))
    s = ServerHandle(db_dir, extra_args=[
        "--max_open_graphs", str(MAX_OPEN),
        "--graph_idle_timeout_s", str(IDLE_TIMEOUT_S),
    ])
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


@pytest.fixture(scope="module")
def names(srv):
    """Register a population larger than the cache and pre-declare its schema.

    Schema is declared up front (sequentially) so the worker threads only do
    data operations -- this avoids a concurrent schema-declare race on the
    same graph masking the real assertion (data ops under eviction). Opens its
    own client because this fixture is module-scoped and must not depend on the
    function-scoped `client` fixture.
    """
    c = srv.rpc()
    try:
        created = ["p2ev_%04d" % i for i in range(POPULATION)]
        for n in created:
            create_graph(c, n)
        listed = set(list_graphs(c))
        missing = [n for n in created if n not in listed]
        assert not missing, "graphs not registered: %s" % (missing[:5],)
        for n in created:
            declare_vertex_label(c, n, LABEL)
    finally:
        try:
            c.logout()
        except Exception:
            pass
    return created


class TestConcurrentAccessDuringEviction:
    def test_population_exceeds_cache(self, names):
        # The whole point: there are more graphs than the cache can hold, so
        # eviction is guaranteed to fire while the workers hammer the store.
        assert len(names) > MAX_OPEN, (
            "test misconfigured: population %d must exceed max_open_graphs %d"
            % (len(names), MAX_OPEN))

    def test_eviction_under_concurrent_load(self, srv, client, names):
        errors = []
        err_lock = threading.Lock()
        stop = threading.Event()
        max_open = {"value": 0}

        def monitor():
            c = srv.rpc()
            try:
                while not stop.is_set():
                    st = _cache_stats(c)
                    try:
                        o = int(st.get("open_graphs", 0))
                    except (TypeError, ValueError):
                        o = 0
                    if o > max_open["value"]:
                        max_open["value"] = o
                    stop.wait(0.05)
            finally:
                try:
                    c.logout()
                except Exception:
                    pass

        def worker(wid):
            c = srv.rpc()
            try:
                for _ in range(ITERS):
                    g = random.choice(names)
                    uid = random.randint(0, 2 ** 31)
                    try:
                        cypher(c,
                               "CREATE (n:%s {id: %d, name: 'x'})" % (LABEL, uid),
                               g)
                        cnt = count_vertices(c, g, LABEL)
                        if cnt <= 0:
                            with err_lock:
                                errors.append(
                                    "zero count after write on %s" % g)
                    except Exception as exc:  # noqa: BLE001
                        with err_lock:
                            errors.append(
                                "worker %d on %s: %r" % (wid, g, exc))
            finally:
                try:
                    c.logout()
                except Exception:
                    pass

        mon = threading.Thread(target=monitor, daemon=True)
        mon.start()
        threads = [threading.Thread(target=worker, args=(i,))
                   for i in range(THREADS)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        stop.set()
        mon.join(timeout=5)

        stats = _cache_stats(client)
        log.info("final cache stats: %s; observed max open=%d",
                 stats, max_open["value"])

        assert not errors, (
            "errors during concurrent eviction:\n%s"
            % "\n".join(errors[:20]))
        assert int(stats.get("evictions", 0)) > 0, (
            "expected evictions > 0 under load; stats=%s" % (stats,))
        # Bounded: never opened the whole population, and stays within the
        # cache size plus the in-flight threads (each thread may hold one open
        # graph that has not yet been evicted).
        assert max_open["value"] <= MAX_OPEN + THREADS + 2, (
            "open graph count grew unbounded: max observed %d "
            "(expected <= %d)" % (max_open["value"], MAX_OPEN + THREADS + 2))
        assert max_open["value"] < POPULATION, (
            "server opened the entire population (%d) instead of a bounded "
            "subset" % POPULATION)
