"""Series performance: ingest and read latency, memory and disk footprint.

Markers:
  perf        every measurement here; selected by `qa/local/run.sh perf full`
  perf_smoke  the same tests with small workloads; selected by
              `qa/local/run.sh perf smoke`

Assertions are correctness-only (counts, values, ordering). Latency, RSS and
storage numbers are written to $QA_PERF_OUT for trend comparison and are never
pass/fail thresholds: wall-clock on a shared machine is not a contract.
Workload sizes come from perf_metrics.pick(smoke, full).

Run:
  qa/local/run.sh perf smoke
  qa/local/run.sh perf full
"""

import json
import logging
import os
import sys
import threading
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import perf_metrics as pm
from server_harness import ServerHandle, cypher

pytestmark = [pytest.mark.perf, pytest.mark.perf_smoke]

log = logging.getLogger(__name__)

BASE_TS = 1704067200000000  # 2024-01-01 00:00:00 UTC, microseconds
DAY_US = 86400 * 1000000


@pytest.fixture(scope="module")
def srv(tmp_path_factory):
    db_dir = str(tmp_path_factory.mktemp("perf_latency"))
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


def stop_sampler(sampler, pid):
    summary = sampler.stop()
    peaks = summary["peaks"]
    return {
        "samples": summary["samples"],
        "rss_peak_kb_sampled": peaks["rss_kb"],
        "vmhwm_kb": pm.rss_peak_kb(pid) or None,
        "vmsize_peak_kb": peaks["vmsize_kb"],
        "threads_peak": peaks["threads"],
        "fds_peak": peaks["fds"],
    }


def emit(section, data):
    path = pm.record(section, data)
    log.info("PERF %s %s", section, json.dumps(data, sort_keys=True))
    if path:
        log.info("PERF recorded %s -> %s", section, path)


def test_ingest_and_range_latency(srv, client):
    symbols = pm.pick(2, 8)
    days = pm.pick(200, 500)
    full_reads = pm.pick(100, 200)
    narrow_reads = pm.pick(50, 100)
    bucket_cap = pm.pick(500, 1000)
    total = symbols * days

    sampler = pm.RssSampler(srv.proc.pid, interval=0.5, with_fds=True).start()
    try:
        cypher(client, "CALL db.createVertexLabel('Latency', 'id', 'id', 'INT64', false)")
        cypher(client, "CALL db.createSeriesField('Latency', 'prices', "
                       "[{name:'close', type:'DOUBLE'}, {name:'volume', type:'INT64'}], "
                       "{bucket_max_points:%d}) YIELD field RETURN field" % bucket_cap)

        # One transaction per point: the per-point RPC is the only write path
        # until a batch API lands (UNWIND+CALL composition does not parse).
        t0 = time.time()
        for s in range(symbols):
            cypher(client, "CREATE (c:Latency {id:%d})" % s)
            for d in range(days):
                cypher(client,
                       "MATCH (c:Latency {id:%d}) CALL series.append(c, 'prices', "
                       "{ts: %d, close: %f, volume: %d}) YIELD written RETURN written"
                       % (s, BASE_TS + d * DAY_US, 100.0 + s + d * 0.01, 1000 + d))
        ingest_s = time.time() - t0

        # Correctness: exact counts for every symbol, and boundary values.
        for s in range(symbols):
            assert cypher(client, "MATCH (c:Latency {id:%d}) "
                                  "RETURN series.count(c, 'prices') AS n" % s) == [
                {"n": days}]
        first = cypher(client, "MATCH (c:Latency {id:0}) RETURN series.at(c, 'prices', %d) AS p"
                       % BASE_TS)[0]["p"]
        last = cypher(client, "MATCH (c:Latency {id:%d}) RETURN series.latest(c, 'prices') AS p"
                      % (symbols - 1))[0]["p"]
        assert first["volume"] == 1000
        assert last["volume"] == 1000 + days - 1
        assert abs(first["close"] - 100.0) < 1e-9
        assert abs(last["close"] - (100.0 + symbols - 1 + (days - 1) * 0.01)) < 1e-9

        # Daily-use read path: full-window reads and single-day range reads.
        full_lat, narrow_lat = [], []
        for i in range(full_reads):
            s = i % symbols
            q0 = time.time()
            rows = cypher(client, "MATCH (c:Latency {id:%d}) "
                                  "RETURN series.range(c, 'prices', %d, %d) AS p"
                          % (s, BASE_TS, BASE_TS + (days - 1) * DAY_US))
            full_lat.append(time.time() - q0)
            assert len(rows[0]["p"]) == days
        for i in range(narrow_reads):
            s = i % symbols
            d = days // 2
            q0 = time.time()
            rows = cypher(client, "MATCH (c:Latency {id:%d}) "
                                  "RETURN series.range(c, 'prices', %d, %d) AS p"
                          % (s, BASE_TS + d * DAY_US, BASE_TS + d * DAY_US))
            narrow_lat.append(time.time() - q0)
            assert len(rows[0]["p"]) == 1

        # Exact payload evidence: every point of every symbol, fingerprinted.
        all_points = []
        for s in range(symbols):
            rows = cypher(client, "MATCH (c:Latency {id:%d}) "
                                  "RETURN series.range(c, 'prices', %d, %d) AS p"
                          % (s, BASE_TS, BASE_TS + (days - 1) * DAY_US))
            pts = rows[0]["p"]
            assert len(pts) == days
            for p in pts:
                all_points.append({"symbol": s, "ts": p["ts"],
                                   "close": p["close"], "volume": p["volume"]})
        assert len(all_points) == total
        digest = pm.fingerprint(all_points)

        memory = stop_sampler(sampler, srv.proc.pid)
        sampler = None
        usage = pm.dir_usage(srv.db_dir)
        emit("latency.ingest_range", {
            "workload": {"symbols": symbols, "points_per_symbol": days,
                         "points": total, "bucket_max_points": bucket_cap},
            "ingest": {"seconds": round(ingest_s, 3),
                       "points_per_s": round(total / ingest_s, 1)},
            "latency": {"full_range": pm.latency_summary(full_lat),
                        "narrow_1day": pm.latency_summary(narrow_lat)},
            "storage": {
                "apparent_bytes": usage["apparent_bytes"],
                "allocated_bytes": usage["allocated_bytes"],
                "files": usage["files"],
                "bytes_per_point_apparent": round(usage["apparent_bytes"] / total, 2),
                "bytes_per_point_allocated": round(usage["allocated_bytes"] / total, 2),
            },
            "memory": memory,
            "integrity": {"points_read": len(all_points),
                          "fingerprint_sha256": digest},
            "durability": "nondurable (server default; LMDB MDB_NOSYNC, durable=false)",
        })
    finally:
        if sampler is not None:
            sampler.stop()


def test_contention_latency(srv):
    # Concurrent ingestion (distinct series), a corrector rewriting one point,
    # and historical scans from the main thread throughout. Writes and scans
    # interleave on the same server, which is the daily mixed workload.
    from test_timeseries import CypherError, Rest

    writers = pm.pick(2, 4)
    per_writer = pm.pick(60, 200)
    correct_rounds = pm.pick(20, 50)

    sampler = pm.RssSampler(srv.proc.pid, interval=0.5).start()
    try:
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

        write_lat, scan_lat = [], []
        lock = threading.Lock()
        retries = [0]
        errors = []

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
                _, rows = scan_rest.cypher(
                    "MATCH (c:Con {id:0}) RETURN series.range(c, 'prices', %d, %d) AS p"
                    % (BASE_TS, BASE_TS + (per_writer - 1) * DAY_US))
                with lock:
                    scan_lat.append(time.time() - q0)
                scans += 1
                assert isinstance(json.loads(rows[0][0]), list)
            except Exception as e:  # noqa: BLE001
                errors.append("scan: %r" % e)
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

        memory = stop_sampler(sampler, srv.proc.pid)
        sampler = None
        emit("latency.contention", {
            "workload": {"writers": writers, "points_per_writer": per_writer,
                         "corrections": correct_rounds},
            "writes": pm.latency_summary(write_lat),
            "scans": pm.latency_summary(scan_lat),
            "scan_count": scans,
            "transport_retries": retries[0],
            "memory": memory,
            "durability": "nondurable (server default)",
        })
    finally:
        if sampler is not None:
            sampler.stop()


def test_fine_grained_cadence(srv, client):
    # 1-second observations spanning several buckets per symbol: 500 x 23,400
    # is the full fine-grained profile; here a scaled proxy that still forces
    # cross-bucket range reads and narrow in-bucket slices.
    symbols = pm.pick(2, 4)
    per_symbol = pm.pick(600, 3600)
    cap = pm.pick(200, 1000)
    full_reads = pm.pick(60, 120)
    narrow_len = min(100, per_symbol // 2)
    narrow_start = per_symbol // 2
    total = symbols * per_symbol

    sampler = pm.RssSampler(srv.proc.pid, interval=0.5, with_fds=True).start()
    try:
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
                       % (s, BASE_TS + p * 1000000, 50.0 + (p % 100) * 0.01))
        ingest_s = time.time() - t0

        for s in range(symbols):
            assert cypher(client, "MATCH (c:Fine {id:%d}) "
                                  "RETURN series.count(c, 'ticks') AS n" % s) == [
                {"n": per_symbol}]

        full_lat, narrow_lat = [], []
        for i in range(full_reads):
            s = i % symbols
            q0 = time.time()
            rows = cypher(client, "MATCH (c:Fine {id:%d}) "
                                  "RETURN series.range(c, 'ticks', %d, %d) AS p"
                          % (s, BASE_TS, BASE_TS + (per_symbol - 1) * 1000000))
            full_lat.append(time.time() - q0)
            assert len(rows[0]["p"]) == per_symbol
            q0 = time.time()
            rows = cypher(
                client,
                "MATCH (c:Fine {id:%d}) RETURN series.range(c, 'ticks', %d, %d) AS p"
                % (s, BASE_TS + narrow_start * 1000000,
                   BASE_TS + (narrow_start + narrow_len - 1) * 1000000))
            narrow_lat.append(time.time() - q0)
            assert len(rows[0]["p"]) == narrow_len

        buckets = (per_symbol + cap - 1) // cap
        memory = stop_sampler(sampler, srv.proc.pid)
        sampler = None
        usage = pm.dir_usage(srv.db_dir)
        emit("latency.fine_grained", {
            "workload": {"symbols": symbols, "points_per_symbol": per_symbol,
                         "cadence_s": 1, "bucket_max_points": cap,
                         "buckets_per_symbol": buckets, "points": total},
            "ingest": {"seconds": round(ingest_s, 3),
                       "points_per_s": round(total / ingest_s, 1)},
            "latency": {"full_span": pm.latency_summary(full_lat),
                        "narrow_slice": pm.latency_summary(narrow_lat)},
            "storage": {
                "apparent_bytes": usage["apparent_bytes"],
                "allocated_bytes": usage["allocated_bytes"],
                "files": usage["files"],
                "bytes_per_point_apparent": round(usage["apparent_bytes"] / total, 2),
                "bytes_per_point_allocated": round(usage["allocated_bytes"] / total, 2),
            },
            "memory": memory,
            "durability": "nondurable (server default)",
        })
    finally:
        if sampler is not None:
            sampler.stop()


def test_startup_restart_with_data(tmp_path):
    # Cold start and warm restart with real series data on disk. `start_seconds`
    # is measured by ServerHandle against the "Server started." log marker, the
    # same boundary the baseline used.
    symbols = 2
    points = pm.pick(50, 200)
    srv = ServerHandle(str(tmp_path / "startup"))
    srv.start()
    cold_start_s = srv.start_seconds
    try:
        c = srv.rpc()
        try:
            cypher(c, "CALL db.createVertexLabel('Startup', 'id', 'id', 'INT64', false)")
            cypher(c, "CALL db.createSeriesField('Startup', 'p', "
                      "[{name:'v', type:'INT64'}], {}) YIELD field RETURN field")
            for s in range(symbols):
                cypher(c, "CREATE (x:Startup {id:%d})" % s)
                for p in range(points):
                    cypher(c, "MATCH (x:Startup {id:%d}) CALL series.append(x, 'p', "
                              "{ts: %d, v: %d}) YIELD written RETURN written"
                              % (s, BASE_TS + p * DAY_US, p))
        finally:
            try:
                c.logout()
            except Exception:
                pass

        stop_s = srv.stop()
        srv.start()
        restart_s = srv.start_seconds
        c = srv.rpc()
        try:
            for s in range(symbols):
                assert cypher(c, "MATCH (x:Startup {id:%d}) "
                                 "RETURN series.count(x, 'p') AS n" % s) == [
                    {"n": points}]
        finally:
            try:
                c.logout()
            except Exception:
                pass

        usage = pm.dir_usage(srv.db_dir)
        emit("startup.restart", {
            "workload": {"symbols": symbols, "points_per_symbol": points,
                         "points": symbols * points},
            "cold_start_s": round(cold_start_s, 4),
            "shutdown_s": round(stop_s, 4),
            "restart_with_data_s": round(restart_s, 4),
            "storage": {"apparent_bytes": usage["apparent_bytes"],
                        "allocated_bytes": usage["allocated_bytes"],
                        "files": usage["files"]},
        })
    finally:
        srv.cleanup()
