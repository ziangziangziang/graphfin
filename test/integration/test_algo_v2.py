import logging
import pytest
import json
import liblgraph_client_python
import math

log = logging.getLogger(__name__)

SERVEROPT = {"cmd":"./lgraph_server -c lgraph_standalone.json --directory ./testdb --port 7072 --rpc_port 9092 --enable_plugin 1",
             "cleanup_dir":["./testdb"]}

CLIENTOPT = {"host":"127.0.0.1:9092", "user":"admin", "password":"73@TuGraph"}

IMPORTOPT = {"cmd":"./lgraph_import --config_file ./data/algo/fb.conf --dir ./testdb --user admin --password 73@TuGraph --graph default --overwrite 1",
             "cleanup_dir":["./testdb", "./.import_tmp"]}


class TestAlgoV2:

    @pytest.mark.parametrize("importor", [IMPORTOPT], indirect=True)
    @pytest.mark.parametrize("server", [SERVEROPT], indirect=True)
    @pytest.mark.parametrize("client", [CLIENTOPT], indirect=True)
    def test_algo_v2(self, importor, server, client):
        algos = ["libbfs_v2.so", "liblcc_v2.so", "liblpa_v2.so", "libpagerank_v2.so", "libsssp_v2.so", "libwcc_v2.so",
                 "libscc_v2.so", "libhits_v2.so", "liblocate_cycle_v2.so", "libspsp_v2.so"]
        algo_dir = "./algo/"
        for algo in algos:
            ret = client.loadProcedure(algo_dir + algo, "CPP", algo.split('.')[0], "SO", "test plugin", True, "v2")
            try:
                assert ret[0]
            except:
                log.info(ret)
                raise

        # Test BFS
        ret = client.callCypher("MATCH (n:node{id: 0}) "
                                "CALL plugin.cpp.libbfs_v2(n) "
                                "YIELD node, parent "
                                "WITH node, parent "
                                "RETURN COUNT(parent)")
        assert ret[0]
        result = json.loads(ret[1])[0].get("COUNT(parent)")
        assert result == 3829

        # Test PageRank
        ret = client.callCypher("CALL plugin.cpp.libpagerank_v2(10) "
                                "YIELD node, weight WITH node, weight "
                                "RETURN MAX(weight)")
        assert ret[0]
        result = json.loads(ret[1])[0].get("MAX(weight)")
        assert math.isclose(result, 0.00939017583256023, rel_tol=1e-5)

        # Test WCC
        ret = client.callCypher("CALL plugin.cpp.libwcc_v2() "
                                "YIELD node, label WITH node, label "
                                "RETURN COUNT(DISTINCT label)")
        assert ret[0]
        result = json.loads(ret[1])[0].get("COUNT(DISTINCT label)")
        assert result == 1

        # Test LPA
        ret = client.callCypher("CALL plugin.cpp.liblpa_v2(10) "
                                "YIELD node, label WITH node, label "
                                "RETURN COUNT(DISTINCT label)")
        assert ret[0]
        result = json.loads(ret[1])[0].get("COUNT(DISTINCT label)")
        assert result == 16

        # Test LCC
        ret = client.callCypher("CALL plugin.cpp.liblcc_v2() "
                                "YIELD node, score WITH node, score "
                                "WHERE node.id = 0 "
                                "RETURN score")
        assert ret[0]
        result = json.loads(ret[1])[0].get("score")
        assert math.isclose(result, 0.041961653145874626, rel_tol=1e-5)

        # Test SSSP
        ret = client.callCypher("MATCH (n:node{id:0}) "
                                "CALL plugin.cpp.libsssp_v2(n) "
                                "YIELD node, distance WITH node, distance "
                                "WHERE node.id = 10 "
                                "RETURN distance")
        assert ret[0]
        result = json.loads(ret[1])[0].get("distance")
        assert result == 1

        ret = client.callCypher("MATCH (n:node) RETURN COUNT(n)")
        assert ret[0]
        total_vertices = json.loads(ret[1])[0].get("COUNT(n)")

        # Test SCC: every vertex gets a component label.
        ret = client.callCypher("CALL plugin.cpp.libscc_v2() "
                                "YIELD node, component "
                                "RETURN COUNT(component), COUNT(DISTINCT component)")
        assert ret[0]
        row = json.loads(ret[1])[0]
        assert row.get("COUNT(component)") == total_vertices
        assert row.get("COUNT(DISTINCT component)") >= 1

        # Test HITS: hub and authority scores are positive on a graph with edges.
        ret = client.callCypher("CALL plugin.cpp.libhits_v2(20, 0.0001) "
                                "YIELD node, authority, hub "
                                "RETURN MAX(authority), MAX(hub)")
        assert ret[0]
        row = json.loads(ret[1])[0]
        assert row.get("MAX(authority)") > 0
        assert row.get("MAX(hub)") > 0

        # Test ring counting.
        ret = client.callCypher("CALL plugin.cpp.liblocate_cycle_v2(3) "
                                "YIELD num_rings, num_vertices, num_edges "
                                "RETURN num_rings, num_vertices, num_edges")
        assert ret[0]
        row = json.loads(ret[1])[0]
        assert row.get("num_rings") >= 0
        assert row.get("num_vertices") == total_vertices
        assert row.get("num_edges") > 0

        # Test SPSP, and that dropping direction never lengthens a path.
        ret = client.callCypher("MATCH (a:node{id:0}), (b:node{id:10}) "
                                "CALL plugin.cpp.libspsp_v2(a, b, true) "
                                "YIELD length RETURN length")
        assert ret[0]
        directed_length = json.loads(ret[1])[0].get("length")
        assert directed_length == 1

        ret = client.callCypher("MATCH (a:node{id:0}), (b:node{id:10}) "
                                "CALL plugin.cpp.libspsp_v2(a, b, false) "
                                "YIELD length RETURN length")
        assert ret[0]
        assert json.loads(ret[1])[0].get("length") <= directed_length
