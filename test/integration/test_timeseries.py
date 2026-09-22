"""Time-series S5 integration evidence: REST wire types, restart durability,
backup/restore round-trip.

Self-contained (ServerHandle pattern): launches its own lgraph_server so it
can restart and copy the data directory without fighting shared fixtures.

REST is driven directly over HTTPX: the bundled TuGraphRestClient login is
stale (it posts `userName`, the server requires `user`), and driving the wire
directly is exactly what pins the client-visible contract.

Run:
  PHASE0_TEST_FILES=test_timeseries.py ci/phase0/run_tests.sh it
"""

import json
import logging
import os
import shutil
import subprocess
import sys

import httpx
import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from phase0_util import (DEFAULT_PASSWORD, DEFAULT_USER, ServerHandle, cypher)

log = logging.getLogger(__name__)


class CypherError(RuntimeError):
    pass


class Rest:
    """Minimal REST client: login with `user`, Bearer token, POST /cypher."""

    def __init__(self, port, retries=30, interval=1.0):
        import time

        self.base = "http://127.0.0.1:%d/" % port
        last = None
        for _ in range(retries):
            try:
                r = httpx.post(self.base + "login",
                               json={"user": DEFAULT_USER, "password": DEFAULT_PASSWORD},
                               timeout=30)
                r.raise_for_status()
                self.headers = {"Authorization": "Bearer " + r.json()["jwt"],
                                "Content-Type": "application/json"}
                return
            except Exception as exc:  # noqa: BLE001 - retry while the server comes up
                last = exc
                time.sleep(interval)
        raise RuntimeError("could not login to REST at %s: %s" % (self.base, last))

    def cypher(self, script, graph="default"):
        """Returns (header_names, rows); rows are positional lists."""
        r = httpx.post(self.base + "cypher",
                       json={"script": script, "graph": graph},
                       headers=self.headers, timeout=120)
        body = r.json()
        if r.status_code != 200:
            raise CypherError("REST cypher failed: %s\n  script: %s"
                              % (body, script))
        header = [c["name"] for c in body.get("header", [])]
        return header, body.get("result", [])


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("series_it"))
    s = ServerHandle(db_dir)
    s.start()
    yield s
    s.cleanup()


@pytest.fixture(scope="module")
def rpc(srv):
    c = srv.rpc()
    yield c
    try:
        c.logout()
    except Exception:
        pass


@pytest.fixture(scope="module")
def rest(srv):
    return Rest(srv.http_port)


def test_rest_series_wire_types(rest):
    # DDL through the REST wire returns an empty-named header and no rows.
    assert rest.cypher("CALL db.createVertexLabel('Wire', 'id', 'id', 'INT64', false)") == (
        [''], [])
    _, rows = rest.cypher(
        "CALL db.createSeriesField('Wire', 'prices', "
        "[{name:'close', type:'DOUBLE'}, {name:'volume', type:'INT64'}], "
        "{bucket_max_points:100}) YIELD field RETURN field")
    assert rows == [["prices"]]
    header, rows = rest.cypher("CREATE (c:Wire {id:1})")
    assert header == ["<SUMMARY>"]

    # Missing measures become null under the full-point replacement contract.
    _, rows = rest.cypher(
        "MATCH (c:Wire {id:1}) CALL series.append(c, 'prices', {ts: "
        "datetime('2024-01-02 00:00:00'), close: 12.5}) YIELD written RETURN written")
    assert rows == [[1]]

    # A point comes back as a JSON-text map over REST (collections cross the
    # RPC/REST boundary as JSON strings by long-standing contract; the RPC
    # JSON layer re-parses them into objects, so RPC clients see structured
    # values while REST clients parse the text). The client parses it into
    # native JSON types here.
    header, rows = rest.cypher(
        "MATCH (c:Wire {id:1}) RETURN series.at(c, 'prices', "
        "datetime('2024-01-02 00:00:00')) AS point")
    assert header == ["point"] and len(rows) == 1
    assert isinstance(rows[0][0], str)
    point = json.loads(rows[0][0])
    assert point["close"] == 12.5 and isinstance(point["close"], float)
    assert point["volume"] is None
    assert point["ts"] == "2024-01-02 00:00:00" and isinstance(point["ts"], str)

    # INT64 extremes round-trip as JSON numbers, not strings.
    _, rows = rest.cypher(
        "MATCH (c:Wire {id:1}) CALL series.update(c, 'prices', 'volume', "
        "datetime('2024-01-02 00:00:00'), 9223372036854775807) "
        "YIELD written RETURN written")
    assert rows == [[1]]
    _, rows = rest.cypher(
        "MATCH (c:Wire {id:1}) RETURN series.at(c, 'prices', "
        "datetime('2024-01-02 00:00:00')) AS point")
    assert json.loads(rows[0][0])["volume"] == 9223372036854775807

    # The summary map shape over the wire (likewise JSON text).
    _, rows = rest.cypher("MATCH (c:Wire {id:1}) RETURN c.prices AS summary")
    summary = json.loads(rows[0][0])
    assert summary["count"] == 1
    assert summary["first"] == "2024-01-02 00:00:00"
    assert summary["last"] == "2024-01-02 00:00:00"
    assert summary["measures"] == ["close", "volume"]

    # Ranges are JSON-text lists of maps; an empty window is `[]`, never null.
    _, rows = rest.cypher(
        "MATCH (c:Wire {id:1}) RETURN series.range(c, 'prices', "
        "datetime('2024-01-01 00:00:00'), datetime('2024-01-03 00:00:00')) AS pts")
    pts = json.loads(rows[0][0])
    assert isinstance(pts, list) and len(pts) == 1 and pts[0]["close"] == 12.5
    _, rows = rest.cypher(
        "MATCH (c:Wire {id:1}) RETURN series.range(c, 'prices', "
        "datetime('2030-01-01 00:00:00'), datetime('2030-01-02 00:00:00')) AS pts")
    assert json.loads(rows[0][0]) == []
    _, rows = rest.cypher("RETURN [] AS e, {} AS m")
    assert json.loads(rows[0][0]) == [] and json.loads(rows[0][1]) == {}

    # A string that looks like JSON stays a string.
    rest.cypher("CALL db.createVertexLabel('Doc', 'id', 'id', 'INT64', false, "
                "'note', 'STRING', true)")
    rest.cypher("CREATE (d:Doc {id:1, note:'{\"a\":1}'})")
    _, rows = rest.cypher("MATCH (d:Doc {id:1}) RETURN d.note AS note")
    assert rows[0][0] == '{"a":1}' and isinstance(rows[0][0], str)

    # Malformed measure values fail over REST and preserve the stored point.
    try:
        rest.cypher(
            "MATCH (c:Wire {id:1}) CALL series.update(c, 'prices', 'close', "
            "datetime('2024-01-02 00:00:00'), [99]) YIELD written RETURN written")
        raise AssertionError("malformed update unexpectedly succeeded")
    except CypherError as e:
        assert "declared type" in str(e)
    _, rows = rest.cypher(
        "MATCH (c:Wire {id:1}) RETURN series.at(c, 'prices', "
        "datetime('2024-01-02 00:00:00')) AS point")
    assert json.loads(rows[0][0])["close"] == 12.5


def test_restart_preserves_series(srv, rpc):
    # Durable restart: committed series data (points + schema) survives a
    # full server stop/start and stays queryable through both clients.
    cypher(rpc, "CALL db.createVertexLabel('Dur', 'id', 'id', 'INT64', false)")
    cypher(rpc, "CALL db.createSeriesField('Dur', 'prices', "
                "[{name:'v', type:'INT64'}], {bucket_max_points:100}) "
                "YIELD field RETURN field")
    cypher(rpc, "CREATE (c:Dur {id:1})")
    for day in range(1, 11):
        cypher(rpc, "MATCH (c:Dur {id:1}) CALL series.append(c, 'prices', "
                    "{ts: datetime('2024-01-%02d 00:00:00'), v: %d}) "
                    "YIELD written RETURN written" % (day, day * 10))
    assert cypher(rpc, "MATCH (c:Dur {id:1}) RETURN series.count(c, 'prices') AS n") == [
        {"n": 10}]
    srv.restart()
    # Same ports survive the restart; reconnect both clients.
    rpc2 = srv.rpc()
    try:
        assert cypher(rpc2, "MATCH (c:Dur {id:1}) RETURN series.count(c, 'prices') AS n") == [
            {"n": 10}]
        latest = cypher(rpc2, "MATCH (c:Dur {id:1}) RETURN series.latest(c, 'prices') AS p")
        assert latest[0]["p"]["v"] == 100
        rest2 = Rest(srv.http_port)
        _, rows = rest2.cypher(
            "MATCH (c:Dur {id:1}) RETURN series.at(c, 'prices', "
            "datetime('2024-01-05 00:00:00')) AS point")
        assert json.loads(rows[0][0])["v"] == 50
    finally:
        try:
            rpc2.logout()
        except Exception:
            pass


def test_bundled_rest_client_login_and_cypher(srv):
    # M4 regression: the bundled TuGraphRestClient posted `userName` while
    # the server requires `user`, and assumed response envelopes the server
    # no longer sends. Login, a Cypher round-trip, and logout must work.
    from TuGraphRestClient import TuGraphRestClient

    c = TuGraphRestClient("http://127.0.0.1:%d/" % srv.http_port,
                          DEFAULT_USER, DEFAULT_PASSWORD)
    try:
        assert c.call_cypher(
            "default", "CALL db.createVertexLabel('Sdk', 'id', 'id', 'INT64', false)") == []
        assert c.call_cypher("default", "CREATE (n:Sdk {id:5})") == [
            ["created 1 vertices, created 0 edges."]]
        assert c.call_cypher("default", "MATCH (n:Sdk) RETURN n.id AS id") == [[5]]
    finally:
        assert c.logout() is True


def test_bolt_series_wire_types(srv):
    # Live Bolt assertions (packstream over TCP, not in-process conversion):
    # collection cells cross as JSON text, scalars as Bolt natives.
    from bolt_driver import BoltClient, BoltError

    b = BoltClient("127.0.0.1", srv.bolt_port, DEFAULT_USER, DEFAULT_PASSWORD)
    try:
        fields, rows, _ = b.run(
            "CALL db.createVertexLabel('Bolt', 'id', 'id', 'INT64', false)")
        fields, rows, _ = b.run(
            "CALL db.createSeriesField('Bolt', 'prices', "
            "[{name:'close', type:'DOUBLE'}, {name:'volume', type:'INT64'}], "
            "{bucket_max_points:100}) YIELD field RETURN field")
        assert rows == [["prices"]]
        b.run("CREATE (c:Bolt {id:1})")
        fields, rows, _ = b.run(
            "MATCH (c:Bolt {id:1}) CALL series.append(c, 'prices', {ts: "
            "datetime('2024-01-02 00:00:00'), close: 12.5, volume: 7}) "
            "YIELD written RETURN written")
        assert rows == [[1]]

        fields, rows, _ = b.run(
            "MATCH (c:Bolt {id:1}) RETURN series.count(c, 'prices') AS n")
        assert fields == ["n"] and rows == [[1]]

        # Point/summary/range cells are JSON text on Bolt (pinned intentional
        # difference from the structured RPC JSON); the client parses them.
        _, rows, _ = b.run(
            "MATCH (c:Bolt {id:1}) RETURN series.at(c, 'prices', "
            "datetime('2024-01-02 00:00:00')) AS point")
        assert isinstance(rows[0][0], str)
        point = json.loads(rows[0][0])
        assert point["close"] == 12.5 and isinstance(point["close"], float)
        assert point["volume"] == 7 and isinstance(point["volume"], int)
        assert point["ts"] == "2024-01-02 00:00:00"

        _, rows, _ = b.run("MATCH (c:Bolt {id:1}) RETURN c.prices AS summary")
        summary = json.loads(rows[0][0])
        assert summary["count"] == 1 and summary["measures"] == ["close", "volume"]

        _, rows, _ = b.run(
            "MATCH (c:Bolt {id:1}) RETURN series.range(c, 'prices', "
            "datetime('2024-01-01 00:00:00'), datetime('2024-01-03 00:00:00')) AS pts")
        assert isinstance(rows[0][0], str)
        assert json.loads(rows[0][0])[0]["close"] == 12.5

        # DATETIME scalar arrives as a Bolt LocalDateTime struct ('d').
        _, rows, _ = b.run("RETURN datetime('2024-01-02 00:00:00') AS dt")
        assert rows[0][0] == ("struct", 0x64, [1704153600, 0])

        # Null, INT64 extremes, empties, and JSON-looking strings.
        _, rows, _ = b.run("RETURN null AS n")
        assert rows[0][0] is None
        _, rows, _ = b.run(
            "RETURN 9223372036854775807 AS mx, 0-9223372036854775807-1 AS mn")
        assert rows[0] == [9223372036854775807, -9223372036854775808]
        _, rows, _ = b.run("RETURN [] AS e, {} AS m")
        assert rows[0][0] == "[]" and rows[0][1] == "{}"
        b.run("CALL db.createVertexLabel('BoltDoc', 'id', 'id', 'INT64', false, "
              "'note', 'STRING', true)")
        b.run("CREATE (d:BoltDoc {id:1, note:'{\"a\":1}'})")
        _, rows, _ = b.run("MATCH (d:BoltDoc {id:1}) RETURN d.note AS note")
        assert rows[0][0] == '{"a":1}'

        # Malformed measures fail over Bolt and preserve the stored point.
        try:
            b.run("MATCH (c:Bolt {id:1}) CALL series.update(c, 'prices', 'close', "
                  "datetime('2024-01-02 00:00:00'), [99]) YIELD written RETURN written")
            raise AssertionError("malformed update unexpectedly succeeded")
        except BoltError as e:
            assert "declared type" in str(e)
        b.reset()  # a failed RUN leaves the session FAILED until RESET
        _, rows, _ = b.run(
            "MATCH (c:Bolt {id:1}) RETURN series.at(c, 'prices', "
            "datetime('2024-01-02 00:00:00')) AS point")
        assert json.loads(rows[0][0])["close"] == 12.5
    finally:
        b.close()


def test_backup_restore_copy(srv, tmp_path):
    # The series table lives in data.mdb, so a stopped file copy carries it.
    # A fresh client: earlier tests restart the server, invalidating the
    # module-scoped connection.
    rpc = srv.rpc()
    cypher(rpc, "CALL db.createVertexLabel('Bak', 'id', 'id', 'INT64', false)")
    cypher(rpc, "CALL db.createSeriesField('Bak', 'prices', "
                "[{name:'v', type:'INT64'}], {}) YIELD field RETURN field")
    cypher(rpc, "CREATE (c:Bak {id:1})")
    cypher(rpc, "MATCH (c:Bak {id:1}) CALL series.append(c, 'prices', "
                "{ts: datetime('2024-06-01 00:00:00'), v: 7}) YIELD written RETURN written")
    srv.stop()
    backup_dir = str(tmp_path / "backup_copy")
    shutil.copytree(srv.db_dir, backup_dir)
    restored = ServerHandle(backup_dir)
    restored.start()
    try:
        c = restored.rpc()
        try:
            assert cypher(c, "MATCH (c:Bak {id:1}) RETURN series.count(c, 'prices') AS n") == [
                {"n": 1}]
            pt = cypher(c, "MATCH (c:Bak {id:1}) RETURN series.at(c, 'prices', "
                           "datetime('2024-06-01 00:00:00')) AS point")
            assert pt[0]["point"]["v"] == 7
        finally:
            try:
                c.logout()
            except Exception:
                pass
    finally:
        restored.cleanup()
    srv.start()


def backup_binary():
    for candidate in (os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                   "..", "..", "build", "output", "lgraph_backup"),
                      "./lgraph_backup"):
        candidate = os.path.abspath(candidate)
        if os.path.isfile(candidate):
            return candidate
    raise RuntimeError("lgraph_backup not found. Build it with ci/phase0/build.sh first.")


def test_sigkill_durable_recovery(tmp_path):
    # Abrupt termination under the configured durable mode: every acknowledged
    # commit (graph + series, same transaction scope) must be fully present
    # after SIGKILL + restart, never partial.
    db_dir = str(tmp_path / "kill_db")
    srv = ServerHandle(db_dir, extra_args=["--durable", "true"])
    srv.start()
    try:
        c = srv.rpc()
        try:
            cypher(c, "CALL db.createVertexLabel('Kill', 'id', 'id', 'INT64', false)")
            cypher(c, "CALL db.createSeriesField('Kill', 'prices', "
                      "[{name:'v', type:'INT64'}], {}) YIELD field RETURN field")
            cypher(c, "CREATE (a:Kill {id:1}), (b:Kill {id:2})")
            for v, base in (("1", 100), ("2", 200)):
                for day in range(1, 6):
                    cypher(c, "MATCH (x:Kill {id:%s}) CALL series.append(x, 'prices', "
                              "{ts: datetime('2024-03-%02d 00:00:00'), v: %d}) "
                              "YIELD written RETURN written" % (v, day, base + day))
        finally:
            try:
                c.logout()
            except Exception:
                pass
        srv.kill()
        srv.start()
        c = srv.rpc()
        try:
            assert cypher(c, "MATCH (x:Kill) RETURN count(x) AS n") == [{"n": 2}]
            for v, base in (("1", 100), ("2", 200)):
                assert cypher(
                    c, "MATCH (x:Kill {id:%s}) RETURN series.count(x, 'prices') AS n"
                    % v) == [{"n": 5}]
                pt = cypher(c, "MATCH (x:Kill {id:%s}) RETURN series.at(x, 'prices', "
                               "datetime('2024-03-03 00:00:00')) AS point" % v)
                assert pt[0]["point"]["v"] == base + 3
        finally:
            try:
                c.logout()
            except Exception:
                pass
    finally:
        srv.cleanup()


def test_tool_backup_restore(tmp_path):
    # The actual backup tool (not just a directory copy) must carry schema
    # and series buckets to the restored database.
    db_dir = str(tmp_path / "tool_src")
    srv = ServerHandle(db_dir)
    srv.start()
    try:
        c = srv.rpc()
        try:
            cypher(c, "CALL db.createVertexLabel('Tool', 'id', 'id', 'INT64', false)")
            cypher(c, "CALL db.createSeriesField('Tool', 'prices', "
                      "[{name:'v', type:'INT64'}], {}) YIELD field RETURN field")
            cypher(c, "CREATE (x:Tool {id:1})")
            cypher(c, "MATCH (x:Tool {id:1}) CALL series.append(x, 'prices', "
                      "{ts: datetime('2024-07-01 00:00:00'), v: 9}) "
                      "YIELD written RETURN written")
        finally:
            try:
                c.logout()
            except Exception:
                pass
        srv.stop()
        backup_dir = str(tmp_path / "tool_backup")
        r = subprocess.run([backup_binary(), "-s", db_dir, "-d", backup_dir],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=300, universal_newlines=True)
        assert r.returncode == 0, r.stderr[-2000:]
        restored = ServerHandle(backup_dir)
        restored.start()
        try:
            c = restored.rpc()
            try:
                assert cypher(
                    c, "MATCH (x:Tool {id:1}) RETURN series.count(x, 'prices') AS n"
                ) == [{"n": 1}]
                pt = cypher(c, "MATCH (x:Tool {id:1}) RETURN series.at(x, 'prices', "
                               "datetime('2024-07-01 00:00:00')) AS point")
                assert pt[0]["point"]["v"] == 9
            finally:
                try:
                    c.logout()
                except Exception:
                    pass
        finally:
            restored.cleanup()
    finally:
        srv.cleanup()
