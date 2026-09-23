"""Post-merge graph lifecycle × series regression; see docs/testing/post-merge.md.

Synthetic finance and telemetry use the same schema/types and oracle. Each test
owns its server and database. This is a small correctness gate, not a RAM benchmark.
"""

import datetime
import os
import shutil

import pytest

from phase0_util import ServerHandle, create_graph, cypher, CypherError


BASE_TS = 1704153600000000
CASES = [("Issuer", "SUPPLIES"), ("Sensor", "FEEDS")]


def seed(client, graph, label, edge, base, after_create=None, create=True):
    if create:
        create_graph(client, graph)
    if after_create is not None:
        after_create(graph)
    cypher(client, "CALL db.createVertexLabel('%s', 'id', 'id', 'INT64', false)" % label, graph)
    cypher(client, "CALL db.createEdgeLabel('%s', '[]')" % edge, graph)
    cypher(client, "CALL db.createSeriesField('%s', 'samples', "
           "[{name:'v', type:'DOUBLE'}, {name:'qty', type:'INT64'}], "
           "{bucket_max_points:2}) YIELD field RETURN field" % label, graph)
    cypher(client, "CALL db.createEdgeSeriesField('%s', 'weights', "
           "[{name:'v', type:'INT64'}], {}) YIELD field RETURN field" % edge, graph)
    cypher(client, "CREATE (a:%s {id:1}), (b:%s {id:2})" % (label, label), graph)
    cypher(client, "MATCH (a:%s {id:1}), (b:%s {id:2}) CREATE (a)-[:%s]->(b)"
           % (label, label, edge), graph)
    # Shuffled timestamps and a missing measure exercise bucket ordering/nulls.
    for i in (2, 0, 1):
        qty = "" if i == 1 else ", qty:%d" % i
        cypher(client, "MATCH (n:%s {id:1}) CALL series.append(n, 'samples', "
               "{ts:%d, v:%s%s}) YIELD written RETURN written"
               % (label, BASE_TS + i * 1000000, float(base + i), qty), graph)
    cypher(client, "MATCH ()-[r:%s]->() CALL series.append(r, 'weights', "
           "{ts:%d, v:%d}) YIELD written RETURN written" % (edge, BASE_TS, base), graph)


def expected(base):
    return [{"ts": (datetime.datetime(2024, 1, 2) + datetime.timedelta(seconds=i))
             .strftime("%Y-%m-%d %H:%M:%S"),
             "v": float(base + i), "qty": None if i == 1 else i} for i in range(3)]


def verify(client, graph, label, edge, base, correction=None):
    want = expected(base)
    if correction is not None:
        want[0]["v"] = correction
    rows = cypher(client, "MATCH (n:%s {id:1}) RETURN series.range(n, 'samples', "
                  "%d, %d) AS points" % (label, BASE_TS, BASE_TS + 2000000), graph)
    assert rows == [{"points": want}]
    assert cypher(client, "MATCH ()-[r:%s]->() RETURN series.at(r, 'weights', %d) AS p"
                  % (edge, BASE_TS), graph) == [
                      {"p": {"ts": "2024-01-02 00:00:00", "v": base}}]


@pytest.mark.parametrize("label,edge", CASES)
def test_tenant_series_eviction_correction_and_recreation(tmp_path, label, edge):
    srv = ServerHandle(str(tmp_path / "db"), extra_args=[
        "--max_open_graphs", "2", "--graph_idle_timeout_s", "1"])
    try:
        srv.start()
        c = srv.rpc()
        graphs = ["tenant_%d" % i for i in range(4)]
        for i, graph in enumerate(graphs):
            seed(c, graph, label, edge, 100 * (i + 1))
        for _ in range(2):
            for i, graph in enumerate(graphs):
                verify(c, graph, label, edge, 100 * (i + 1))
        stats = cypher(c, "CALL dbms.graph.cacheStats()")[0]
        assert int(stats["evictions"]) > 0, stats
        assert int(stats["open_graphs"]) <= 2, stats
        update = ("MATCH (n:%s {id:1}) CALL series.update_cas(n, 'samples', 'v', "
                  "%d, 100.0, 999.0) YIELD written RETURN written") % (label, BASE_TS)
        assert cypher(c, update, graphs[0]) == [{"written": 1}]
        assert cypher(c, update, graphs[0]) == [{"written": 0}]
        with pytest.raises(CypherError):
            cypher(c, "MATCH (n:%s {id:1}) CALL series.update(n, 'samples', 'v', "
                   "%d, toFloat('Infinity')) YIELD written RETURN written" % (label, BASE_TS),
                   graphs[0])
        c.logout()
        srv.restart()
        c = srv.rpc()
        for i, graph in enumerate(graphs):
            verify(c, graph, label, edge, 100 * (i + 1), 999.0 if i == 0 else None)
        cypher(c, "CALL dbms.graph.deleteGraph('%s')" % graphs[0])
        seed(c, graphs[0], label, edge, 700)
        verify(c, graphs[0], label, edge, 700)
        verify(c, graphs[1], label, edge, 200)
        c.logout()
    finally:
        srv.kill()


@pytest.mark.parametrize("label,edge", CASES)
def test_snapshot_restores_vertex_and_edge_series_across_graphs(tmp_path, label, edge):
    srv = ServerHandle(str(tmp_path / "source"), extra_args=["--max_open_graphs", "2"])
    restored = None
    try:
        srv.start()
        c = srv.rpc()
        for i in range(3):
            seed(c, "snap_%d" % i, label, edge, 100 * (i + 1))
        cypher(c, "CALL dbms.takeSnapshot()")
        root = os.path.join(srv.db_dir, "snapshot")
        snapshots = sorted(os.listdir(root))
        assert snapshots, "snapshot procedure produced no snapshot"
        snapshot = os.path.join(root, snapshots[-1])
        assert os.path.isfile(os.path.join(snapshot, ".meta", "data.mdb"))
        destination = str(tmp_path / "restored")
        shutil.copytree(snapshot, destination)
        for i in range(3):
            verify(c, "snap_%d" % i, label, edge, 100 * (i + 1))
        c.logout()
        srv.stop()
        restored = ServerHandle(destination, extra_args=["--max_open_graphs", "2"])
        restored.start()
        r = restored.rpc()
        for i in range(3):
            verify(r, "snap_%d" % i, label, edge, 100 * (i + 1))
        r.logout()
    finally:
        if restored is not None:
            restored.kill()
        srv.kill()
