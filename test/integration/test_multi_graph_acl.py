"""Phase 0: per-graph ACL correctness across multiple graphs.

Verifies that graph-level access levels are enforced per graph:
a role granted FULL on one graph and READ on another must be able to write the
first, only read the second, and not reach a third at all.

ACL is server-wide storage but per-graph in effect
(src/db/acl.h:139-163, src/db/acl.cpp:76-107).

Run:
  PHASE0_TEST_FILES=test_multi_graph_acl.py dev/phase0/run_tests.sh it
"""

import logging

import pytest

from phase0_util import (CypherError, ServerHandle, count_vertices,
    create_graph_if_missing, cypher, cypher_ok, scalar)

log = logging.getLogger(__name__)

VERTEX_SCHEMA = (
    "CALL db.createVertexLabel('aclitem', 'id', "
    "'id', 'INT64', false, 'name', 'string', true)"
)

ROLE = "phase0_acl_role"
USER = "phase0_acl_user"
PASSWORD = "phase0_password"
GRAPHS = ("acl_full", "acl_read", "acl_none")


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("mg_acl"))
    s = ServerHandle(db_dir)
    s.start()
    yield s
    s.cleanup()


# Function-scoped: test_acl_survives_restart restarts the server, which
# invalidates clients created before the restart.
@pytest.fixture
def client(srv):
    c = srv.rpc()
    yield c
    try:
        c.logout()
    except Exception:
        pass


@pytest.fixture(scope="module")
def acl_setup(srv):
    """One-time ACL fixture: graphs, schema, role, user and per-graph grants.

    Idempotent so it is safe to re-run against a reused server.
    """
    c = srv.rpc()
    try:
        for g in GRAPHS:
            create_graph_if_missing(c, g)
            try:
                cypher(c, VERTEX_SCHEMA, g)
            except CypherError:
                pass
            if count_vertices(c, g, "aclitem") == 0:
                cypher(c, "CREATE (n:aclitem {id: 1, name: '%s'})" % g, g)

        for stmt in (
            "CALL dbms.security.createRole('%s', 'phase0 acl test')" % ROLE,
            "CALL dbms.security.modRoleAccessLevel('%s', "
            "{acl_full:'FULL', acl_read:'READ', acl_none:'NONE'})" % ROLE,
            "CALL dbms.security.createUser('%s', '%s')" % (USER, PASSWORD),
            "CALL dbms.security.addUserRoles('%s', ['%s'])" % (USER, ROLE),
        ):
            try:
                cypher(c, stmt)
            except CypherError:
                pass
    finally:
        c.logout()
    return True


@pytest.fixture
def user_client(srv, acl_setup):
    """A client authenticated as a non-admin user with per-graph grants."""
    import liblgraph_client_python
    c = liblgraph_client_python.client(
        "127.0.0.1:%d" % srv.rpc_port, USER, PASSWORD)
    yield c
    try:
        c.logout()
    except Exception:
        pass


class TestMultiGraphAcl:
    def test_full_access_can_read_and_write(self, user_client, acl_setup):
        assert cypher_ok(user_client, "MATCH (n:aclitem) RETURN n", "acl_full")
        assert cypher_ok(
            user_client, "CREATE (n:aclitem {id: 2, name: 'written'})", "acl_full")

    def test_read_access_can_read(self, user_client, acl_setup):
        assert cypher_ok(user_client, "MATCH (n:aclitem) RETURN n", "acl_read")

    def test_read_access_cannot_write(self, user_client, acl_setup):
        assert not cypher_ok(
            user_client, "CREATE (n:aclitem {id: 3, name: 'nope'})", "acl_read"), \
            "READ-only access must not permit writes"

    def test_none_access_is_denied(self, user_client, acl_setup):
        assert not cypher_ok(user_client, "MATCH (n:aclitem) RETURN n", "acl_none"), \
            "NONE access must deny reads"
        assert not cypher_ok(
            user_client, "CREATE (n:aclitem {id: 4, name: 'nope'})", "acl_none"), \
            "NONE access must deny writes"

    def test_ungranted_graph_is_not_reachable(self, user_client, acl_setup):
        # The role was never granted the default graph.
        assert not cypher_ok(user_client, "MATCH (n) RETURN n", "default"), \
            "ungranted graphs must not be reachable"

    def test_write_denial_does_not_mutate_the_graph(self, client, user_client,
                                                    acl_setup):
        before = count_vertices(client, "acl_read", "aclitem")
        cypher_ok(user_client, "CREATE (n:aclitem {id: 99, name: 'denied'})",
                  "acl_read")
        after = count_vertices(client, "acl_read", "aclitem")
        assert before == after, (
            "a denied write changed the graph: %s -> %s" % (before, after))

    def test_admin_retains_access_to_all_graphs(self, client, acl_setup):
        for g in GRAPHS:
            assert cypher_ok(client, "MATCH (n:aclitem) RETURN n", g)

    def test_acl_survives_restart(self, srv, acl_setup):
        """Roles, users and per-graph grants are persisted server-wide."""
        srv.restart()

        fresh = srv.rpc()
        try:
            assert cypher_ok(fresh, "MATCH (n:aclitem) RETURN n", "acl_full")
        finally:
            fresh.logout()

        import liblgraph_client_python
        uc = liblgraph_client_python.client(
            "127.0.0.1:%d" % srv.rpc_port, USER, PASSWORD)
        try:
            assert cypher_ok(uc, "MATCH (n:aclitem) RETURN n", "acl_full"), \
                "FULL grant lost across restart"
            assert not cypher_ok(uc, "MATCH (n:aclitem) RETURN n", "acl_none"), \
                "NONE grant lost across restart"
        finally:
            uc.logout()
