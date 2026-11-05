"""Series payload integrity across graceful restart and SIGKILL.

This is correctness, not performance: every recovered point must carry exactly
the value that was written, and recovery must never expose a hole or a torn
point. It complements test_timeseries.py (which checks counts and sampled
points) by comparing the complete payload of every symbol.

Durability semantics asserted here:
  * graceful stop/start  -> every acknowledged write is present;
  * durable mode + SIGKILL -> every acknowledged write is present;
  * default mode + SIGKILL -> LMDB runs with MDB_NOSYNC and durable=false, so
    the tail of the write stream may be lost. What must hold is consistency:
    recovered data is an exact prefix of the writes (no holes, no duplicates,
    no modified values), and the server is usable afterwards.

Run:
  qa/local/run.sh it
  QA_TEST_FILES=test_series_recovery_integrity.py qa/local/run.sh it
"""

import logging
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from server_harness import ServerHandle, cypher

log = logging.getLogger(__name__)

SYMBOLS = 2
POINTS = 200
BASE_TS = 1704067200000000  # 2024-01-01 00:00:00 UTC, microseconds
STEP_US = 1000000


def write_series(srv, label, symbols, points):
    # Symbol-major order matters for the default-mode prefix check: symbol 1 is
    # written only after symbol 0 is complete.
    c = srv.rpc()
    try:
        cypher(c, "CALL db.createVertexLabel('%s', 'id', 'id', 'INT64', false)"
               % label)
        cypher(c, "CALL db.createSeriesField('%s', 'p', "
                  "[{name:'v', type:'INT64'}], {}) YIELD field RETURN field"
               % label)
        for s in range(symbols):
            cypher(c, "CREATE (x:%s {id:%d})" % (label, s))
            for p in range(points):
                cypher(c, "MATCH (x:%s {id:%d}) CALL series.append(x, 'p', "
                          "{ts: %d, v: %d}) YIELD written RETURN written"
                          % (label, s, BASE_TS + p * STEP_US, p))
    finally:
        try:
            c.logout()
        except Exception:
            pass


def read_series(srv, label, symbol):
    c = srv.rpc()
    try:
        rows = cypher(c, "MATCH (x:%s {id:%d}) RETURN series.range(x, 'p', %d, %d) AS p"
                      % (label, symbol, BASE_TS, BASE_TS + (POINTS - 1) * STEP_US))
        pts = rows[0]["p"]
        count = cypher(c, "MATCH (x:%s {id:%d}) RETURN series.count(x, 'p') AS n"
                       % (label, symbol))
        assert count == [{"n": len(pts)}], (label, symbol, count, len(pts))
        return [p["v"] for p in pts]
    finally:
        try:
            c.logout()
        except Exception:
            pass


def assert_prefix(values, label):
    assert values == list(range(len(values))), (
        "%s: recovered payload is not an exact prefix: %r..." % (label, values[:20]))


def test_graceful_restart_preserves_exact_payload(tmp_path):
    srv = ServerHandle(str(tmp_path / "graceful"))
    srv.start()
    try:
        write_series(srv, "RecGrace", SYMBOLS, POINTS)
        srv.restart()
        for s in range(SYMBOLS):
            values = read_series(srv, "RecGrace", s)
            assert values == list(range(POINTS)), \
                "graceful restart lost or altered data in symbol %d" % s
    finally:
        srv.cleanup()


def test_default_mode_sigkill_recovers_consistent_prefix(tmp_path):
    srv = ServerHandle(str(tmp_path / "kill_default"))
    srv.start()
    try:
        write_series(srv, "RecKill", SYMBOLS, POINTS)
        srv.kill()  # SIGKILL: no graceful shutdown, no flush
        srv.start()

        recovered = []
        for s in range(SYMBOLS):
            values = read_series(srv, "RecKill", s)
            assert_prefix(values, "symbol %d" % s)
            recovered.append(len(values))
        log.info("default-mode SIGKILL: recovered %r of %d points per symbol",
                 recovered, POINTS)

        assert sum(recovered) > 0, "no committed data survived SIGKILL"
        # Writes are symbol-major: if symbol 0 lost its tail, symbol 1 cannot
        # have been written yet, so it must have recovered nothing.
        if recovered[0] < POINTS:
            assert recovered[1] == 0, (
                "recovery exposed a hole across symbols: %r" % recovered)

        # The database must remain writable after recovery.
        c = srv.rpc()
        try:
            append_at = recovered[0] if recovered[0] < POINTS else POINTS
            cypher(c, "MATCH (x:RecKill {id:0}) CALL series.append(x, 'p', "
                      "{ts: %d, v: %d}) YIELD written RETURN written"
                   % (BASE_TS + append_at * STEP_US, append_at))
            assert cypher(c, "MATCH (x:RecKill {id:0}) "
                             "RETURN series.count(x, 'p') AS n") == [
                {"n": append_at + 1}]
        finally:
            try:
                c.logout()
            except Exception:
                pass
    finally:
        srv.cleanup()


def test_durable_mode_sigkill_preserves_acknowledged_writes(tmp_path):
    srv = ServerHandle(str(tmp_path / "kill_durable"),
                       extra_args=["--durable", "true"])
    srv.start()
    try:
        write_series(srv, "RecDurable", SYMBOLS, POINTS)
        srv.kill()
        srv.start()
        for s in range(SYMBOLS):
            values = read_series(srv, "RecDurable", s)
            assert values == list(range(POINTS)), (
                "durable mode lost acknowledged writes in symbol %d: got %d of %d"
                % (s, len(values), POINTS))
    finally:
        srv.cleanup()
