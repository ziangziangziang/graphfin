"""Sustained mixed-load soak: append, range-read and correction traffic.

Marker `soak` only; never selected by the correctness gates. Duration comes
from QA_SOAK_MINUTES (default 60). The test records write/read latency, RSS,
fd and thread footprint per minute, and then proves that a graceful restart
after the soak preserves every acknowledged point and correction exactly.

There are no latency thresholds here: the value is the timeline, which makes
slow leaks, degradation and long-run corruption visible without making a
shared runner flaky. Correctness assertions are exact.

Run:
  qa/local/run.sh soak 30
  QA_SOAK_MINUTES=2 qa/local/run.sh soak
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

pytestmark = [pytest.mark.soak]

log = logging.getLogger(__name__)

BASE_TS = 1704067200000000
STEP_US = 1000000
SYMBOLS = 2
INITIAL_CORRECTION_POINTS = 100
APPEND_INTERVAL_S = 0.1
READ_INTERVAL_S = 0.05
CORRECT_INTERVAL_S = 0.5
READ_WINDOW = 50
CORRECTION_SYMBOL = 90


def to_rest_json(rows):
    return json.loads(rows[0][0])


def test_soak_mixed_workload(tmp_path):
    minutes = float(os.environ.get("QA_SOAK_MINUTES", "60"))
    duration_s = minutes * 60.0

    from test_timeseries import Rest

    srv = ServerHandle(str(tmp_path / "soak"))
    srv.start()
    stop = threading.Event()
    lock = threading.Lock()
    errors = []
    append_lat = []
    read_lat = []
    writes = [0] * SYMBOLS
    reads = [0]
    corrections = [0]
    corrected = {}
    buckets = {}

    try:
        setup = Rest(srv.http_port)
        setup.cypher("CALL db.createVertexLabel('Soak', 'id', 'id', 'INT64', false)")
        setup.cypher("CALL db.createSeriesField('Soak', 'prices', "
                     "[{name:'v', type:'INT64'}], {bucket_max_points:500}) "
                     "YIELD field RETURN field")
        for s in range(SYMBOLS):
            setup.cypher("CREATE (x:Soak {id:%d})" % s)
        setup.cypher("CREATE (x:Soak {id:%d})" % CORRECTION_SYMBOL)
        for p in range(INITIAL_CORRECTION_POINTS):
            setup.cypher("MATCH (x:Soak {id:%d}) CALL series.append(x, 'prices', "
                         "{ts: %d, v: %d}) YIELD written RETURN written"
                         % (CORRECTION_SYMBOL, BASE_TS + p * STEP_US, p))

        def writer(s):
            try:
                rest = Rest(srv.http_port)
            except Exception as e:  # noqa: BLE001
                errors.append("writer %d login: %r" % (s, e))
                return
            i = 0
            while not stop.is_set():
                q0 = time.monotonic()
                try:
                    rest.cypher(
                        "MATCH (x:Soak {id:%d}) CALL series.append(x, 'prices', "
                        "{ts: %d, v: %d}) YIELD written RETURN written"
                        % (s, BASE_TS + i * STEP_US, i))
                except Exception as e:  # noqa: BLE001 - collected, asserted below
                    errors.append("writer %d at %d: %r" % (s, i, e))
                    return
                with lock:
                    append_lat.append(time.monotonic() - q0)
                    writes[s] += 1
                i += 1
                stop.wait(APPEND_INTERVAL_S)

        def reader():
            try:
                rest = Rest(srv.http_port)
            except Exception as e:  # noqa: BLE001
                errors.append("reader login: %r" % e)
                return
            j = 0
            while not stop.is_set():
                s = j % SYMBOLS
                with lock:
                    count = writes[s]
                if count > 0:
                    lo = BASE_TS + max(0, count - READ_WINDOW) * STEP_US
                    hi = BASE_TS + count * STEP_US
                    q0 = time.monotonic()
                    try:
                        _, rows = rest.cypher(
                            "MATCH (x:Soak {id:%d}) RETURN series.range(x, 'prices', "
                            "%d, %d) AS p" % (s, lo, hi))
                        pts = to_rest_json(rows)
                    except Exception as e:  # noqa: BLE001
                        errors.append("reader %d: %r" % (s, e))
                        return
                    with lock:
                        read_lat.append(time.monotonic() - q0)
                        reads[0] += 1
                    if not 1 <= len(pts) <= READ_WINDOW + 1:
                        errors.append("reader %d: implausible window %d"
                                      % (s, len(pts)))
                        return
                j += 1
                stop.wait(READ_INTERVAL_S)

        def corrector():
            try:
                rest = Rest(srv.http_port)
            except Exception as e:  # noqa: BLE001
                errors.append("corrector login: %r" % e)
                return
            k = 0
            while not stop.is_set():
                target = k % INITIAL_CORRECTION_POINTS
                value = 1000000 + k
                try:
                    rest.cypher(
                        "MATCH (x:Soak {id:%d}) CALL series.update(x, 'prices', 'v', "
                        "%d, %d) YIELD written RETURN written"
                        % (CORRECTION_SYMBOL, BASE_TS + target * STEP_US, value))
                except Exception as e:  # noqa: BLE001
                    errors.append("corrector at %d: %r" % (k, e))
                    return
                with lock:
                    corrected[target] = value
                    corrections[0] += 1
                k += 1
                stop.wait(CORRECT_INTERVAL_S)

        def monitor():
            pid = srv.proc.pid
            while not stop.is_set():
                snap = pm.process_snapshot(pid, with_fds=True)
                if snap:
                    elapsed = time.monotonic() - t_start
                    idx = int(elapsed // 60)
                    with lock:
                        b = buckets.setdefault(idx, {"minute": idx, "samples": 0,
                                                     "rss_kb_max": 0,
                                                     "vmsize_kb_max": 0,
                                                     "fds_max": 0,
                                                     "threads_max": 0})
                        b["samples"] += 1
                        b["rss_kb_max"] = max(b["rss_kb_max"], snap["rss_kb"])
                        b["vmsize_kb_max"] = max(b["vmsize_kb_max"],
                                                 snap["vmsize_kb"])
                        b["fds_max"] = max(b["fds_max"], snap["fds"])
                        b["threads_max"] = max(b["threads_max"], snap["threads"])
                stop.wait(5.0)

        threads = [threading.Thread(target=writer, args=(s,))
                   for s in range(SYMBOLS)]
        threads.append(threading.Thread(target=reader))
        threads.append(threading.Thread(target=corrector))
        threads.append(threading.Thread(target=monitor))

        t_start = time.monotonic()
        log.info("SOAK start duration_min=%.2f symbols=%d append_interval_s=%.2f",
                 minutes, SYMBOLS, APPEND_INTERVAL_S)
        for t in threads:
            t.start()
        deadline = t_start + duration_s
        while time.monotonic() < deadline and not errors:
            time.sleep(min(5.0, max(0.1, deadline - time.monotonic())))
            with lock:
                done = sum(writes)
            log.info("SOAK %.1fs/%ds writes=%d reads=%d corrections=%d errors=%d",
                     time.monotonic() - t_start, duration_s, done, reads[0],
                     corrections[0], len(errors))
        stop.set()
        for t in threads:
            t.join(timeout=60)
        elapsed = time.monotonic() - t_start
        assert not errors, errors[:5]

        # All acknowledged points are queryable before and after a restart.
        pre = {}
        for s in range(SYMBOLS):
            _, rows = setup.cypher(
                "MATCH (x:Soak {id:%d}) "
                "RETURN series.count(x, 'prices') AS n" % s)
            assert rows == [[writes[s]]]
            pre[s] = writes[s]
        _, rows = setup.cypher(
            "MATCH (x:Soak {id:%d}) "
            "RETURN series.count(x, 'prices') AS n" % CORRECTION_SYMBOL)
        assert rows == [[INITIAL_CORRECTION_POINTS]]

        shutdown_s = srv.stop()
        srv.start()
        restart_s = srv.start_seconds

        all_points = []
        post = {}
        for s in range(SYMBOLS):
            c = srv.rpc()
            try:
                rows = cypher(c, "MATCH (x:Soak {id:%d}) "
                                 "RETURN series.range(x, 'prices', %d, %d) AS p"
                              % (s, BASE_TS, BASE_TS + writes[s] * STEP_US))
                pts = rows[0]["p"]
                post[s] = len(pts)
                assert [p["v"] for p in pts] == list(range(writes[s])), (
                    "soak payload corrupted for symbol %d" % s)
                all_points.extend({"symbol": s, "ts": p["ts"], "v": p["v"]}
                                  for p in pts)
            finally:
                try:
                    c.logout()
                except Exception:
                    pass
        assert post == pre, "graceful restart changed counts: %r -> %r" % (pre, post)

        c = srv.rpc()
        try:
            rows = cypher(c, "MATCH (x:Soak {id:%d}) "
                             "RETURN series.range(x, 'prices', %d, %d) AS p"
                          % (CORRECTION_SYMBOL, BASE_TS,
                             BASE_TS + INITIAL_CORRECTION_POINTS * STEP_US))
            pts = rows[0]["p"]
            assert len(pts) == INITIAL_CORRECTION_POINTS
            for target, p in enumerate(pts):
                expected = corrected.get(target, target)
                assert p["v"] == expected, (
                    "correction %d: got %r expected %r"
                    % (target, p["v"], expected))
        finally:
            try:
                c.logout()
            except Exception:
                pass

        # Still usable after the soak and restart.
        c = srv.rpc()
        try:
            cypher(c, "MATCH (x:Soak {id:0}) CALL series.append(x, 'prices', "
                      "{ts: %d, v: %d}) YIELD written RETURN written"
                   % (BASE_TS + writes[0] * STEP_US, writes[0]))
            assert cypher(c, "MATCH (x:Soak {id:0}) "
                             "RETURN series.count(x, 'prices') AS n") == [
                {"n": writes[0] + 1}]
        finally:
            try:
                c.logout()
            except Exception:
                pass

        with lock:
            if corrected:
                correction_checks = len(corrected)
            else:
                correction_checks = 0
            timeline = [buckets[k] for k in sorted(buckets)]
            summary = {
                "duration_min": round(elapsed / 60.0, 3),
                "target_min": minutes,
                "workload": {"symbols": SYMBOLS,
                             "append_interval_s": APPEND_INTERVAL_S,
                             "read_interval_s": READ_INTERVAL_S,
                             "correct_interval_s": CORRECT_INTERVAL_S,
                             "initial_correction_points": INITIAL_CORRECTION_POINTS},
                "writes": {"total": sum(writes), "per_symbol": writes,
                           "per_s": round(sum(writes) / elapsed, 2)},
                "reads": {"total": reads[0],
                          "per_s": round(reads[0] / elapsed, 2)},
                "corrections": {"total": corrections[0],
                                "targets": correction_checks},
                "latency": {"append": pm.latency_summary(append_lat),
                            "range": pm.latency_summary(read_lat)},
                "memory": {"minutes": timeline,
                           "rss_start_kb": timeline[0]["rss_kb_max"] if timeline else None,
                           "rss_end_kb": timeline[-1]["rss_kb_max"] if timeline else None,
                           "rss_peak_kb": max([b["rss_kb_max"] for b in timeline] or [0]),
                           "fds_peak": max([b["fds_max"] for b in timeline] or [0]),
                           "threads_peak": max([b["threads_max"] for b in timeline] or [0])},
                "restart": {"shutdown_s": round(shutdown_s, 4),
                            "restart_s": round(restart_s, 4)},
                "integrity": {"points_read": len(all_points),
                              "fingerprint_sha256": pm.fingerprint(all_points),
                              "counts_before": pre, "counts_after": post},
                "durability": "nondurable (server default; graceful stop flushes)",
            }
        path = pm.record("soak", summary)
        log.info("SOAK result %s", json.dumps(summary, sort_keys=True))
        if path:
            log.info("SOAK recorded -> %s", path)
    finally:
        stop.set()
        srv.cleanup()
