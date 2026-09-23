"""Phase 0: CRUD, transactions, indexes and schemas across multiple graphs.

Verifies that per-graph stores are genuinely independent: schema, data, indexes
and failed transactions in one graph must not affect another.

Run:
  PHASE0_TEST_FILES=test_multi_graph_crud.py dev/phase0/run_tests.sh it
"""

import logging

import pytest

from phase0_util import (CypherError, ServerHandle, count_vertices,
    create_graph, create_graph_if_missing, cypher, scalar)

log = logging.getLogger(__name__)

VERTEX_SCHEMA = (
    "CALL db.createVertexLabel('person', 'id', "
    "'id', 'INT64', false, 'name', 'string', true)"
)
EDGE_SCHEMA = "CALL db.createEdgeLabel('knows', '[[\"person\",\"person\"]]')"


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("mg_crud"))
    s = ServerHandle(db_dir)
    s.start()
    yield s
    s.cleanup()


# Function-scoped: tests that restart the server invalidate older clients.
@pytest.fixture
def client(srv):
    c = srv.rpc()
    yield c
    try:
        c.logout()
    except Exception:
        pass


def init_schema(client, graph):
    """Create the graph if needed, then declare its schema. Idempotent."""
    create_graph_if_missing(client, graph)
    for stmt in (VERTEX_SCHEMA, EDGE_SCHEMA):
        try:
            cypher(client, stmt, graph)
        except CypherError:
            # Already declared on a previous run against this server.
            pass


class TestMultiGraphCrud:
    def test_schema_is_per_graph(self, client):
        create_graph(client, "crud_schema_a")
        create_graph(client, "crud_schema_b")
        init_schema(client, "crud_schema_a")

        # Graph A has the label and can be written to.
        cypher(client, "CREATE (n:person {id: 1, name: 'a'})", "crud_schema_a")
        assert count_vertices(client, "crud_schema_a", "person") == 1

        # Graph B has no such label: the label must not exist there.
        created_in_b = False
        try:
            cypher(client, "CREATE (n:person {id: 1, name: 'b'})", "crud_schema_b")
            created_in_b = True
        except Exception:
            pass
        assert not created_in_b, (
            "label 'person' leaked from crud_schema_a into crud_schema_b")

    def test_create_read_update_delete(self, client):
        init_schema(client, "crud_basic")
        cypher(client, "CREATE (n:person {id: 1, name: 'alice'})", "crud_basic")
        cypher(client, "CREATE (n:person {id: 2, name: 'bob'})", "crud_basic")
        assert count_vertices(client, "crud_basic", "person") == 2

        cypher(client, "MATCH (n:person {id: 1}) SET n.name = 'alice2'",
               "crud_basic")
        assert scalar(client,
                      "MATCH (n:person {id: 1}) RETURN n.name",
                      "crud_basic") == "alice2"

        cypher(client, "MATCH (n:person {id: 2}) DELETE n", "crud_basic")
        assert count_vertices(client, "crud_basic", "person") == 1

    def test_relationship_crud(self, client):
        init_schema(client, "crud_rel")
        cypher(client, "CREATE (a:person {id: 1, name: 'a'})", "crud_rel")
        cypher(client, "CREATE (b:person {id: 2, name: 'b'})", "crud_rel")
        cypher(client,
               "MATCH (a:person {id:1}), (b:person {id:2}) "
               "CREATE (a)-[:knows]->(b)",
               "crud_rel")
        assert scalar(client, "MATCH ()-[e:knows]->() RETURN count(e)",
                      "crud_rel") == 1
        cypher(client, "MATCH ()-[e:knows]->() DELETE e", "crud_rel")
        assert scalar(client, "MATCH ()-[e:knows]->() RETURN count(e)",
                      "crud_rel") == 0

    def test_unique_index_is_enforced(self, client):
        init_schema(client, "crud_index")
        cypher(client, "CALL db.addIndex('person', 'name', true)", "crud_index")
        cypher(client, "CREATE (n:person {id: 1, name: 'unique'})", "crud_index")

        # A second vertex with the same indexed value must be rejected.
        with pytest.raises(Exception):
            cypher(client, "CREATE (n:person {id: 2, name: 'unique'})", "crud_index")
        assert count_vertices(client, "crud_index", "person") == 1

    def test_duplicate_primary_key_is_atomic(self, client):
        """A failed statement must not leave a partial write behind."""
        init_schema(client, "crud_atomic")
        cypher(client, "CREATE (n:person {id: 1, name: 'first'})", "crud_atomic")
        assert count_vertices(client, "crud_atomic", "person") == 1

        with pytest.raises(Exception):
            cypher(client, "CREATE (n:person {id: 1, name: 'second'})", "crud_atomic")

        assert count_vertices(client, "crud_atomic", "person") == 1
        # The original value must be untouched.
        assert scalar(client, "MATCH (n:person {id:1}) RETURN n.name",
                      "crud_atomic") == "first"

    def test_cross_graph_isolation_under_writes(self, client):
        init_schema(client, "crud_iso_a")
        init_schema(client, "crud_iso_b")
        for i in range(20):
            cypher(client, "CREATE (n:person {id: %d, name: 'a%d'})" % (i, i),
                   "crud_iso_a")
        for i in range(5):
            cypher(client, "CREATE (n:person {id: %d, name: 'b%d'})" % (i, i),
                   "crud_iso_b")
        assert count_vertices(client, "crud_iso_a", "person") == 20
        assert count_vertices(client, "crud_iso_b", "person") == 5

    def test_multi_graph_writes_persist_across_restart(self, srv, client):
        for g, n in (("crud_persist_a", 3), ("crud_persist_b", 7)):
            init_schema(client, g)
            for i in range(n):
                cypher(client, "CREATE (n:person {id: %d, name: 'x%d'})" % (i, i), g)

        srv.restart()

        fresh = srv.rpc()
        try:
            assert count_vertices(fresh, "crud_persist_a", "person") == 3
            assert count_vertices(fresh, "crud_persist_b", "person") == 7
        finally:
            fresh.logout()
