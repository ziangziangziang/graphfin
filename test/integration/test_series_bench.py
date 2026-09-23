"""Series benchmark methodology (M0): reproducible baselines, never gates.

Measures ingest throughput, range-query latency percentiles, stored
bytes/point, and server peak RSS for a synthetic OHLCV workload, and logs a
baseline table with hardware, seed, schema, durability mode, and hashes.
Asserts only workload correctness (counts/values); wall-clock thresholds
must never gate unit tests, and these numbers are methodology baselines,
not production capacity claims.

Run:
  PHASE0_TEST_FILES=test_series_bench.py dev/phase0/run_tests.sh it
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


def test_bench_contention_latency(srv):
    # Concurrent ingestion (4 writers, distinct series) + one corrector
    # rewriting a single point + main-thread historical scans throughout.
    # Records p50/p95/p99 for writes and scans, conflict retries, peak RSS.
    # Correctness asserted; latency only logged (never gated).
    import threading

    from test_timeseries import CypherError, Rest

    writers, per_writer, correct_rounds = 4, 200, 50
    write_lat, scan_lat = [], []
    lock = threading.Lock()
    retries = [0]
    errors = []

    setup = Rest(srv.http_port)
    setup.cypher("CALL db.createVertexLabel('Con', 'id', 'id', 'INT64', false)")
    setup.cypher("CALL db.createSeriesField('Con', 'prices', "
                 "[{name:'v', type:'INT64'}], {bucket_max_points:1000}) "
                 "YIELD field RETURN field")
    for w in range(writers):
        setup.cypher("CREATE (c:Con {id:%d})" % w)
    setup.cypher("CREATE (c:Con {id:99})")
    setup.cypher("MATCH (c:Con {id:99}) CALL series.append(c, 'prices', "
                 "{ts: %d, v: 0}) YIELD written RETURN written" % BASE_TS)

    stop = [False]

    def writer(w):
        rest = Rest(srv.http_port)
        try:
            for p in range(per_writer):
                q0 = time.time()
                for _ in range(20):
                    try:
                        rest.cypher(
                            "MATCH (c:Con {id:%d}) CALL series.append(c, 'prices', "
                            "{ts: %d, v: %d}) YIELD written RETURN written"
                            % (w, BASE_TS + p * DAY_US, p))
                        break
                    except CypherError:
                        raise
                    except Exception:
                        retries[0] += 1
                else:
                    errors.append("writer %d point %d: retries exhausted" % (w, p))
                    return
                with lock:
                    write_lat.append(time.time() - q0)
        except Exception as e:  # noqa: BLE001 - collected, asserted below
            errors.append("writer %d: %r" % (w, e))

    def corrector():
        rest = Rest(srv.http_port)
        try:
            for i in range(correct_rounds):
                rest.cypher(
                    "MATCH (c:Con {id:99}) CALL series.update(c, 'prices', 'v', "
                    "%d, %d) YIELD written RETURN written" % (BASE_TS, i))
        except Exception as e:  # noqa: BLE001
            errors.append("corrector: %r" % e)

    threads = [threading.Thread(target=writer, args=(w,)) for w in range(writers)]
    threads.append(threading.Thread(target=corrector))
    for t in threads:
        t.start()
    scan_rest = Rest(srv.http_port)
    scans = 0
    while any(t.is_alive() for t in threads):
        q0 = time.time()
        try:
            rows = scan_rest.cypher(
                "MATCH (c:Con {id:0}) RETURN series.range(c, 'prices', %d, %d) AS p"
                % (BASE_TS, BASE_TS + (per_writer - 1) * DAY_US))
            with lock:
                scan_lat.append(time.time() - q0)
            scans += 1
            json.loads(rows[0][0])
        except Exception:
            pass
        time.sleep(0.05)
    for t in threads:
        t.join()
    assert not errors, errors[:3]

    check = Rest(srv.http_port)
    for w in range(writers):
        _, rows = check.cypher(
            "MATCH (c:Con {id:%d}) RETURN series.count(c, 'prices') AS n" % w)
        assert rows == [[per_writer]], (w, rows)
    _, rows = check.cypher(
        "MATCH (c:Con {id:99}) RETURN series.at(c, 'prices', %d) AS p" % BASE_TS)
    assert json.loads(rows[0][0])["v"] == correct_rounds - 1

    def pct(xs, q):
        xs = sorted(xs)
        return round(xs[min(len(xs) - 1, int(len(xs) * q))], 4) if xs else None

    peak_kb = server_rss_peak_kb(srv.proc.pid)
    for k, v in [
        ("contention_writes", writers * per_writer),
        ("contention_write_p50_s", pct(write_lat, 0.50)),
        ("contention_write_p95_s", pct(write_lat, 0.95)),
        ("contention_write_p99_s", pct(write_lat, 0.99)),
        ("contention_scans", scans),
        ("contention_scan_p50_s", pct(scan_lat, 0.50)),
        ("contention_scan_p95_s", pct(scan_lat, 0.95)),
        ("contention_scan_p99_s", pct(scan_lat, 0.99)),
        ("contention_transport_retries", retries[0]),
        ("server_peak_rss_mb", round(peak_kb / 1024, 1) if peak_kb else None),
    ]:
        log.info("BENCH %s=%s", k, v)


def test_bench_fine_grained_1s_cadence(srv, client):
    # Methodology for the plan's fine-grained profile (500 x 23,400 1-second
    # observations = 11.7M points): here 4 symbols x 3600 points exercise
    # multi-bucket spans (cap 1000 -> ~4 buckets/symbol), cross-bucket range
    # reads, and narrow-slice reads. Numbers are methodology baselines only.
    symbols, per_symbol, cap = 4, 3600, 1000
    base = 1704067200000000
    cypher(client, "CALL db.createVertexLabel('Fine', 'id', 'id', 'INT64', false)")
    cypher(client, "CALL db.createSeriesField('Fine', 'ticks', "
                   "[{name:'px', type:'DOUBLE'}], {bucket_max_points:%d}) "
                   "YIELD field RETURN field" % cap)
    t0 = time.time()
    for s in range(symbols):
        cypher(client, "CREATE (c:Fine {id:%d})" % s)
        for p in range(per_symbol):
            cypher(client,
                   "MATCH (c:Fine {id:%d}) CALL series.append(c, 'ticks', "
                   "{ts: %d, px: %f}) YIELD written RETURN written"
                   % (s, base + p * 1000000, 50.0 + (p % 100) * 0.01))
    ingest_s = time.time() - t0
    total = symbols * per_symbol
    for s in (0, symbols - 1):
        assert cypher(client, "MATCH (c:Fine {id:%d}) "
                              "RETURN series.count(c, 'ticks') AS n" % s) == [
            {"n": per_symbol}]
    # Full-span reads cross ~4 buckets; narrow slices hit one.
    full_lat, narrow_lat = [], []
    for i in range(60):
        s = i % symbols
        q0 = time.time()
        rows = cypher(client, "MATCH (c:Fine {id:%d}) RETURN series.range(c, 'ticks', %d, %d) AS p"
                      % (s, base, base + (per_symbol - 1) * 1000000))
        full_lat.append(time.time() - q0)
        assert len(rows[0]["p"]) == per_symbol
        q0 = time.time()
        rows = cypher(client, "MATCH (c:Fine {id:%d}) RETURN series.range(c, 'ticks', %d, %d) AS p"
                      % (s, base + 1000 * 1000000, base + 1099 * 1000000))
        narrow_lat.append(time.time() - q0)
        assert len(rows[0]["p"]) == 100

    def pct(xs, q):
        xs = sorted(xs)
        return round(xs[min(len(xs) - 1, int(len(xs) * q))], 4)

    peak_kb = server_rss_peak_kb(srv.proc.pid)
    for k, v in [
        ("fine_points", total),
        ("fine_buckets_per_symbol", per_symbol // cap + 1),
        ("fine_ingest_throughput_pts", round(total / ingest_s, 1)),
        ("fine_fullspan_p50_s", pct(full_lat, 0.50)),
        ("fine_fullspan_p95_s", pct(full_lat, 0.95)),
        ("fine_narrow100_p50_s", pct(narrow_lat, 0.50)),
        ("fine_narrow100_p95_s", pct(narrow_lat, 0.95)),
        ("fine_db_dir_bytes_per_point", round(du_bytes(srv.db_dir) / total, 2)),
        ("server_peak_rss_mb", round(peak_kb / 1024, 1) if peak_kb else None),
    ]:
        log.info("BENCH %s=%s", k, v)


def test_bench_storage_amplification(tmp_path):
    # Storage amplification across bucket caps: same workload under caps 100
    # and 1000, bytes/point from directory size, plus full-range rewrite
    # timings as a rewrite-cost proxy. (Exact rewritten-bytes counters await
    # the M3 query-diagnostics instrumentation.)
    from phase0_util import ServerHandle

    results = {}
    for cap in (100, 1000):
        db_dir = str(tmp_path / ("amp_%d" % cap))
        srv = ServerHandle(db_dir)
        srv.start()
        try:
            c = srv.rpc()
            try:
                cypher(c, "CALL db.createVertexLabel('Amp', 'id', 'id', 'INT64', false)")
                cypher(c, "CALL db.createSeriesField('Amp', 'p', "
                          "[{name:'v', type:'INT64'}], {bucket_max_points:%d}) "
                          "YIELD field RETURN field" % cap)
                t0 = time.time()
                for s in range(4):
                    cypher(c, "CREATE (x:Amp {id:%d})" % s)
                    for p in range(500):
                        cypher(c, "MATCH (x:Amp {id:%d}) CALL series.append(x, 'p', "
                                  "{ts: %d, v: %d}) YIELD written RETURN written"
                                  % (s, 1704067200000000 + p * 86400000000, p))
                ingest_s = time.time() - t0
                total = 4 * 500
                for s in range(4):
                    assert cypher(c, "MATCH (x:Amp {id:%d}) "
                                     "RETURN series.count(x, 'p') AS n" % s) == [{"n": 500}]
                # Rewrite every point once (correction workload).
                t0 = time.time()
                for s in range(4):
                    for p in range(500):
                        cypher(c, "MATCH (x:Amp {id:%d}) CALL series.update(x, 'p', 'v', "
                                  "%d, %d) YIELD written RETURN written"
                                  % (s, 1704067200000000 + p * 86400000000, p + 1))
                rewrite_s = time.time() - t0
                results[cap] = (total / ingest_s, total / rewrite_s,
                                du_bytes(db_dir) / total)
            finally:
                try:
                    c.logout()
                except Exception:
                    pass
        finally:
            srv.cleanup()
    for cap, (ingest, rewrite, bpp) in sorted(results.items()):
        log.info("BENCH amp_cap=%d ingest_pts=%s rewrite_pts=%s db_dir_bytes_per_point=%s",
                 cap, round(ingest, 1), round(rewrite, 1), round(bpp, 2))


def test_bench_overbudget_eviction_churn(tmp_path):
    # Methodology for the over-budget profile: more graphs than the open
    # budget (6 graphs, max_open_graphs=2) with continuous cross-graph churn
    # reads, so eviction + reopen interleave with query traffic. Records
    # per-read latencies and the eviction counter. Small-scale stand-in: the
    # full profile needs data past the memory budget, which this
    # environment cannot hold.
    import httpx

    from phase0_util import create_graph, free_port

    graphs = ["ch%d" % i for i in range(6)]
    db_dir = str(tmp_path / "overbudget_db")
    monitor_port = free_port()
    srv = ServerHandle(db_dir, extra_args=["--max_open_graphs", "2",
                                           "--graph_idle_timeout_s", "1",
                                           "--monitor_host",
                                           "127.0.0.1:%d" % monitor_port])
    srv.start()
    try:
        c = srv.rpc()
        try:
            for g in graphs:
                create_graph(c, g)
                cypher(c, "CALL db.createVertexLabel('E', 'id', 'id', 'INT64', false)",
                       graph=g)
                cypher(c, "CALL db.createSeriesField('E', 'p', "
                          "[{name:'v', type:'INT64'}], {}) YIELD field RETURN field",
                       graph=g)
                for s in range(2):
                    cypher(c, "CREATE (x:E {id:%d})" % s, graph=g)
                    for p in range(200):
                        cypher(c, "MATCH (x:E {id:%d}) CALL series.append(x, 'p', "
                                  "{ts: %d, v: %d}) YIELD written RETURN written"
                                  % (s, 1704067200000000 + p * 86400000000, p),
                               graph=g)
            lat = []
            rounds = 4
            # Drop refs and outwait the idle task (5s minimum period) so
            # eviction fires; then churn with a fresh client.
            try:
                c.logout()
            except Exception:
                pass
            time.sleep(8)
            c = srv.rpc()
            for _ in range(rounds):
                for g in graphs:
                    for s in range(2):
                        q0 = time.time()
                        rows = cypher(
                            c, "MATCH (x:E {id:%d}) RETURN series.range(x, 'p', %d, %d) AS p"
                            % (s, 1704067200000000, 1704067200000000 + 199 * 86400000000),
                            graph=g)
                        lat.append(time.time() - q0)
                        assert len(rows[0]["p"]) == 200
            lat.sort()

            def pct(q):
                return round(lat[min(len(lat) - 1, int(len(lat) * q))], 4)

            metrics = httpx.get("http://127.0.0.1:%d/metrics" % monitor_port,
                                timeout=30).text
            evictions = 0
            for line in metrics.splitlines():
                if 'metric="evictions"' in line and not line.startswith("#"):
                    evictions = int(float(line.rsplit(" ", 1)[1]))
            for k, v in [
                ("overbudget_graphs", len(graphs)),
                ("overbudget_points", len(graphs) * 2 * 200),
                ("overbudget_reads", len(lat)),
                ("overbudget_read_p50_s", pct(0.50)),
                ("overbudget_read_p95_s", pct(0.95)),
                ("overbudget_evictions", evictions),
            ]:
                log.info("BENCH %s=%s", k, v)
            assert evictions >= 1
        finally:
            try:
                c.logout()
            except Exception:
                pass
    finally:
        srv.cleanup()
