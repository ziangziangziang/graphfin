"""Series performance: storage amplification and over-budget eviction churn.

Markers:
  perf        every measurement here; selected by `qa/local/run.sh perf full`
  perf_smoke  the same tests with small workloads; selected by
              `qa/local/run.sh perf smoke`

Assertions are correctness-only (counts, values, eviction happened). Storage
and latency numbers are recorded to $QA_PERF_OUT for trend comparison, never
used as pass/fail thresholds.

Run:
  qa/local/run.sh perf smoke
  qa/local/run.sh perf full
"""

import json
import logging
import os
import sys
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import perf_metrics as pm
from server_harness import (ServerHandle, create_graph, cypher, free_port)

pytestmark = [pytest.mark.perf, pytest.mark.perf_smoke]

log = logging.getLogger(__name__)

BASE_TS = 1704067200000000
DAY_US = 86400 * 1000000


def emit(section, data):
    path = pm.record(section, data)
    log.info("PERF %s %s", section, json.dumps(data, sort_keys=True))
    if path:
        log.info("PERF recorded %s -> %s", section, path)


def test_storage_amplification(tmp_path):
    # Same workload under bucket caps 100 and 1000: bytes per point (apparent
    # and allocated), plus a full rewrite pass as a rewrite-cost proxy.
    symbols = 4
    points = pm.pick(150, 500)
    results = {}
    for cap in (100, 1000):
        db_dir = str(tmp_path / ("amp_%d" % cap))
        srv = ServerHandle(db_dir)
        srv.start()
        sampler = pm.RssSampler(srv.proc.pid, interval=0.5, with_fds=True).start()
        try:
            c = srv.rpc()
            try:
                cypher(c, "CALL db.createVertexLabel('Amp', 'id', 'id', 'INT64', false)")
                cypher(c, "CALL db.createSeriesField('Amp', 'p', "
                          "[{name:'v', type:'INT64'}], {bucket_max_points:%d}) "
                          "YIELD field RETURN field" % cap)
                t0 = time.time()
                for s in range(symbols):
                    cypher(c, "CREATE (x:Amp {id:%d})" % s)
                    for p in range(points):
                        cypher(c, "MATCH (x:Amp {id:%d}) CALL series.append(x, 'p', "
                                  "{ts: %d, v: %d}) YIELD written RETURN written"
                                  % (s, BASE_TS + p * DAY_US, p))
                ingest_s = time.time() - t0
                total = symbols * points
                for s in range(symbols):
                    assert cypher(c, "MATCH (x:Amp {id:%d}) "
                                     "RETURN series.count(x, 'p') AS n" % s) == [
                        {"n": points}]

                # Rewrite every point once (correction workload).
                t0 = time.time()
                for s in range(symbols):
                    for p in range(points):
                        cypher(c, "MATCH (x:Amp {id:%d}) CALL series.update(x, 'p', 'v', "
                                  "%d, %d) YIELD written RETURN written"
                                  % (s, BASE_TS + p * DAY_US, p + 1))
                rewrite_s = time.time() - t0
                mid = points // 2
                row = cypher(c, "MATCH (x:Amp {id:0}) RETURN series.at(x, 'p', %d) AS p"
                             % (BASE_TS + mid * DAY_US))[0]["p"]
                assert row["v"] == mid + 1

                summary = sampler.stop()
                sampler = None
                usage = pm.dir_usage(db_dir)
                results["cap_%d" % cap] = {
                    "bucket_max_points": cap,
                    "buckets_per_symbol": (points + cap - 1) // cap,
                    "points": total,
                    "ingest": {"seconds": round(ingest_s, 3),
                               "points_per_s": round(total / ingest_s, 1)},
                    "rewrite": {"seconds": round(rewrite_s, 3),
                                "points_per_s": round(total / rewrite_s, 1)},
                    "storage": {
                        "apparent_bytes": usage["apparent_bytes"],
                        "allocated_bytes": usage["allocated_bytes"],
                        "files": usage["files"],
                        "bytes_per_point_apparent":
                            round(usage["apparent_bytes"] / total, 2),
                        "bytes_per_point_allocated":
                            round(usage["allocated_bytes"] / total, 2),
                    },
                    "memory": {
                        "rss_peak_kb_sampled": summary["peaks"]["rss_kb"],
                        "vmhwm_kb": pm.rss_peak_kb(srv.proc.pid) or None,
                        "fds_peak": summary["peaks"]["fds"],
                    },
                }
            finally:
                try:
                    c.logout()
                except Exception:
                    pass
        finally:
            if sampler is not None:
                sampler.stop()
            srv.cleanup()

    emit("storage.amplification", {
        "workload": {"symbols": symbols, "points_per_symbol": points,
                     "points": symbols * points},
        "caps": results,
        "durability": "nondurable (server default)",
    })


def test_overbudget_eviction_churn(tmp_path):
    # More open graphs than the open budget, with continuous cross-graph reads
    # so eviction and reopen interleave with query traffic. The small-scale
    # stand-in for the memory-budget profile: 6 graphs against a budget of 2.
    import httpx

    graph_count = pm.pick(4, 6)
    budget = 2
    points = pm.pick(100, 200)
    symbols = 2
    rounds = pm.pick(2, 4)
    graphs = ["ch%d" % i for i in range(graph_count)]

    monitor_port = free_port()
    srv = ServerHandle(
        str(tmp_path / "overbudget_db"),
        extra_args=["--max_open_graphs", str(budget),
                    "--graph_idle_timeout_s", "1",
                    "--monitor_host", "127.0.0.1:%d" % monitor_port])
    srv.start()
    sampler = pm.RssSampler(srv.proc.pid, interval=0.5, with_fds=True).start()
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
                for s in range(symbols):
                    cypher(c, "CREATE (x:E {id:%d})" % s, graph=g)
                    for p in range(points):
                        cypher(c, "MATCH (x:E {id:%d}) CALL series.append(x, 'p', "
                                  "{ts: %d, v: %d}) YIELD written RETURN written"
                                  % (s, BASE_TS + p * DAY_US, p), graph=g)

            # Drop the client's graph references and outwait the idle task so
            # the budget really evicts, then churn with a fresh client.
            try:
                c.logout()
            except Exception:
                pass
            time.sleep(8)
            c = srv.rpc()

            lat = []
            for _ in range(rounds):
                for g in graphs:
                    for s in range(symbols):
                        q0 = time.time()
                        rows = cypher(
                            c, "MATCH (x:E {id:%d}) RETURN series.range(x, 'p', %d, %d) AS p"
                            % (s, BASE_TS, BASE_TS + (points - 1) * DAY_US),
                            graph=g)
                        lat.append(time.time() - q0)
                        assert len(rows[0]["p"]) == points

            metrics = httpx.get("http://127.0.0.1:%d/metrics" % monitor_port,
                                timeout=30).text
            evictions = 0
            for line in metrics.splitlines():
                if 'metric="evictions"' in line and not line.startswith("#"):
                    evictions = int(float(line.rsplit(" ", 1)[1]))

            memory = sampler.stop()
            sampler = None
            emit("storage.eviction_churn", {
                "workload": {"graphs": graph_count, "open_budget": budget,
                             "symbols_per_graph": symbols,
                             "points_per_symbol": points,
                             "reads": len(lat)},
                "reads": pm.latency_summary(lat),
                "evictions": evictions,
                "memory": {
                    "rss_peak_kb_sampled": memory["peaks"]["rss_kb"],
                    "vmhwm_kb": pm.rss_peak_kb(srv.proc.pid) or None,
                    "fds_peak": memory["peaks"]["fds"],
                },
                "durability": "nondurable (server default)",
            })
            assert evictions >= 1, "graph budget was never enforced"
        finally:
            try:
                c.logout()
            except Exception:
                pass
    finally:
        if sampler is not None:
            sampler.stop()
        srv.cleanup()
