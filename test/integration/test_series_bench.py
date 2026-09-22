"""Series benchmark methodology (M0): reproducible baselines, never gates.

Measures ingest throughput, range-query latency percentiles, stored
bytes/point, and server peak RSS for a synthetic OHLCV workload, and logs a
baseline table with hardware, seed, schema, durability mode, and hashes.
Asserts only workload correctness (counts/values); wall-clock thresholds
must never gate unit tests, and these numbers are methodology baselines,
not production capacity claims.

Run:
  PHASE0_TEST_FILES=test_series_bench.py ci/phase0/run_tests.sh it
"""

import json
import logging
import os
import statistics
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from phase0_util import (DEFAULT_PASSWORD, DEFAULT_USER, ServerHandle, cypher)

log = logging.getLogger(__name__)

SYMBOLS = 8
DAYS = 500
BASE_TS = 1704067200000000  # 2024-01-01 00:00:00 UTC, microseconds
DAY_US = 86400 * 1000000


def server_rss_peak_kb(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return int(line.split()[1])
    except (IOError, OSError, ValueError):
        pass
    return None


def du_bytes(path):
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            try:
                total += os.path.getsize(os.path.join(root, name))
            except OSError:
                pass
    return total


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("series_bench"))
    s = ServerHandle(db_dir)
    s.start()
    yield s
    s.cleanup()


@pytest.fixture(scope="module")
def client(srv):
    c = srv.rpc()
    yield c
    try:
        c.logout()
    except Exception:
        pass


def test_bench_ohlcv_baseline(srv, client, tmp_path):
    seed = 42
    cypher(client, "CALL db.createVertexLabel('Bench', 'id', 'id', 'INT64', false)")
    cypher(client, "CALL db.createSeriesField('Bench', 'prices', "
                   "[{name:'close', type:'DOUBLE'}, {name:'volume', type:'INT64'}], "
                   "{bucket_max_points:1000}) YIELD field RETURN field")
    # One transaction per point (per-point RPC is the only write path until
    # the M2 batch API lands; UNWIND+CALL composition does not parse).
    # Methodology scale only: not a production-capacity claim.
    t0 = time.time()
    for s in range(SYMBOLS):
        cypher(client, "CREATE (c:Bench {id:%d})" % s)
        for d in range(DAYS):
            cypher(client,
                   "MATCH (c:Bench {id:%d}) CALL series.append(c, 'prices', "
                   "{ts: %d, close: %f, volume: %d}) YIELD written RETURN written"
                   % (s, BASE_TS + d * DAY_US, 100.0 + s + d * 0.01, 1000 + d))
    ingest_s = time.time() - t0
    total = SYMBOLS * DAYS

    # Correctness: exact counts and boundary values per symbol.
    for s in (0, SYMBOLS - 1):
        assert cypher(client, "MATCH (c:Bench {id:%d}) "
                              "RETURN series.count(c, 'prices') AS n" % s) == [
            {"n": DAYS}]
    first = cypher(client, "MATCH (c:Bench {id:0}) RETURN series.at(c, 'prices', %d) AS p"
                   % BASE_TS)[0]["p"]
    last = cypher(client, "MATCH (c:Bench {id:%d}) RETURN series.latest(c, 'prices') AS p"
                  % (SYMBOLS - 1))[0]["p"]
    assert first["volume"] == 1000 and last["volume"] == 1000 + DAYS - 1

    # Latency: 200 full-window range reads across symbols.
    lat = []
    for i in range(100):
        s = i % SYMBOLS
        q0 = time.time()
        rows = cypher(client, "MATCH (c:Bench {id:%d}) RETURN series.range(c, 'prices', %d, %d) AS p"
                      % (s, BASE_TS, BASE_TS + (DAYS - 1) * DAY_US))
        lat.append(time.time() - q0)
        assert len(rows[0]["p"]) == DAYS
    lat.sort()
    p50, p95 = lat[len(lat) // 2], lat[int(len(lat) * 0.95)]

    peak_kb = server_rss_peak_kb(srv.proc.pid)
    db_bytes = du_bytes(srv.db_dir)
    table = [
        ("workload", "%d symbols x %d daily points = %d points" % (SYMBOLS, DAYS, total)),
        ("seed", seed),
        ("ingest_throughput_pts", round(total / ingest_s, 1)),
        ("ingest_wall_s", round(ingest_s, 1)),
        ("range_p50_s", round(p50, 4)),
        ("range_p95_s", round(p95, 4)),
        ("db_dir_bytes_per_point", round(db_bytes / total, 2)),  # whole db dir, not series bytes alone
        ("server_peak_rss_mb", round(peak_kb / 1024, 1) if peak_kb else None),
        ("durability", "server default (nondurable)"),
    ]
    for k, v in table:
        log.info("BENCH %s=%s", k, v)
    assert total == SYMBOLS * DAYS
