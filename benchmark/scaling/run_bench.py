#!/usr/bin/env python3
"""Phase 0 graph-count scaling benchmark for TuGraph.

Runs one complete measurement cycle per graph count (default: 1, 100, 1000,
4000) and writes a machine-readable JSON result plus a human-readable report.

Per graph count N the cycle is:

  1. fresh db_dir
  2. start server                        -> startup seconds
  3. create graphs up to N               -> per-op create latency, bucketed by
                                            current graph count (captures the
                                            O(N) copy-on-write lifecycle cost)
  4. steady-state resource snapshot      -> RSS, VmSize, fds, threads, mappings
  5. storage per empty graph
  6. first-query latency (cold)
  7. repeated-query latency              -> p50/p95/p99
  8. transaction throughput              -> reads/s and writes/s
  9. online snapshot                     -> duration + size
 10. stop server                         -> shutdown seconds
 11. restart with N graphs on disk       -> startup seconds  <-- key scaling number
 12. restore snapshot into a fresh dir   -> copy+startup seconds, verified
 13. delete all extra graphs             -> per-op delete latency, bucketed
 14. stop server

Every phase is individually timed and individually error-isolated: a failure is
recorded with its message and the observed resource peaks, then the harness
continues where that still makes sense. Results are flushed to disk after every
phase so a long run is never lost.

Standard library only (Python 3.6 compatible).
"""

import argparse
import json
import os
import shutil
import statistics
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import envprobe                # noqa: E402
import procmetrics             # noqa: E402
import restclient              # noqa: E402
import serverctl               # noqa: E402

BENCH_VERSION = "1.0.0"
CLOCK = time.perf_counter

DEFAULT_GRAPHS = [1, 100, 1000, 4000]
READ_QUERY = "MATCH (n) RETURN count(n)"
# 'v' is the unique primary field of the phase0bench label, so the value must
# differ per write. See ensure_bench_schema() and the throughput worker.
WRITE_QUERY_FMT = "CREATE (n:phase0bench {v: %d})"
WRITE_KEY_STRIDE = 1000000000

CREATE_BUCKETS = [(1, 10), (10, 100), (100, 1000), (1000, 10000), (10000, 100001)]


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

def pctl(values, p):
    if not values:
        return None
    vs = sorted(values)
    if len(vs) == 1:
        return vs[0]
    k = (len(vs) - 1) * (p / 100.0)
    lo = int(k)
    hi = min(lo + 1, len(vs) - 1)
    return vs[lo] + (vs[hi] - vs[lo]) * (k - lo)


def latency_summary(values):
    if not values:
        return {"n": 0}
    ms = [v * 1000.0 for v in values]
    return {
        "n": len(ms),
        "mean_ms": round(statistics.mean(ms), 3),
        "p50_ms": round(pctl(ms, 50), 3),
        "p95_ms": round(pctl(ms, 95), 3),
        "p99_ms": round(pctl(ms, 99), 3),
        "max_ms": round(max(ms), 3),
        "min_ms": round(min(ms), 3),
        "sum_seconds": round(sum(values), 3),
    }


def bucket_label(n):
    for lo, hi in CREATE_BUCKETS:
        if lo <= n < hi:
            return "%d-%d" % (lo, hi - 1)
    return ">last"


def graph_name(i):
    return "g%05d" % i


def _phase(phases, *names):
    """True if any of the given aliases was requested on the command line.

    Phase names have drifted historically ("start" vs "startup"), so accept
    both rather than silently skipping a phase the user asked for.
    """
    return any(n in phases for n in names)


def eprint(*a):
    sys.stderr.write(" ".join(str(x) for x in a) + "\n")
    sys.stderr.flush()


class Recorder(object):
    """Holds the full result document and flushes it after every change."""

    def __init__(self, path):
        self.path = path
        self.doc = {
            "phase0_bench_version": BENCH_VERSION,
            "started_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "runs": {},
        }

    def set(self, key, value):
        self.doc[key] = value

    def flush(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.doc, f, indent=2, sort_keys=True)
        os.replace(tmp, self.path)


def phase(rec, run, name, fn):
    """Run a phase, timing it and capturing any failure. Never raises."""
    eprint("  [%s] %s ..." % (run.get("graphs_target"), name))
    t0 = CLOCK()
    entry = {"seconds": None, "error": None}
    try:
        result = fn()
        if isinstance(result, dict):
            entry.update(result)
    except Exception as exc:  # noqa: BLE001 - deliberately broad
        entry["error"] = "%s: %s" % (type(exc).__name__, exc)
        entry["traceback_tail"] = _tb_tail()
        eprint("  [%s] %s FAILED: %s" % (run.get("graphs_target"), name, entry["error"]))
    entry["wall_seconds"] = round(CLOCK() - t0, 3)
    run[name] = entry
    rec.flush()
    return entry


def _tb_tail(n=6):
    import traceback
    return "\n".join(traceback.format_exc().splitlines()[-n:])


# ---------------------------------------------------------------------------
# one graph-count cycle
# ---------------------------------------------------------------------------

class Cycle(object):
    def __init__(self, args, rec, n):
        self.args = args
        self.rec = rec
        self.n = n
        self.run = {"graphs_target": n}
        self.db_dir = os.path.join(args.data_dir, "db_%d" % n)
        self.restore_dir = os.path.join(args.data_dir, "restore_%d" % n)
        self.snapshot_dir = os.path.join(args.data_dir, "snapshot_%d" % n)
        self.server = None
        self.rest = None
        self.created = []          # graph names created beyond 'default'
        self.create_latencies = []
        self.create_buckets = {}
        self.delete_latencies = []
        self.delete_buckets = {}

    # -- server lifecycle ---------------------------------------------------

    def make_server(self):
        s = serverctl.Server(
            binary=self.args.binary,
            db_dir=self.db_dir,
            config_path=self.args.config,
            extra_args=self.args.server_arg,
            log_path=os.path.join(self.args.data_dir, "logs",
                                  "server_%d_%d.log" % (self.n, int(time.time()))),
            startup_timeout=self.args.startup_timeout,
            sampler_interval=self.args.sample_interval,
        )
        return s

    def new_rest(self):
        if self.rest is not None:
            self.rest.close()
        self.rest = restclient.RestClient(
            "127.0.0.1", self.server.http_port, timeout=self.args.http_timeout)
        return self.rest

    def do_startup(self):
        self.server = self.make_server()
        secs = self.server.start()
        restclient.wait_until_ready("127.0.0.1", self.server.http_port,
                                    timeout=self.args.http_timeout)
        self.new_rest()
        return {"seconds": round(secs, 4),
                "http_port": self.server.http_port,
                "rpc_port": self.server.rpc_port,
                "bolt_port": self.server.bolt_port}

    def live(self):
        return self.server is not None and self.server.alive()

    def ensure_live(self):
        if not self.live():
            raise RuntimeError("server is not running")
        return self.server

    # -- graph creation -----------------------------------------------------

    def create_graphs(self):
        if self.n <= 1:
            return {"created": 0, "note": "only the auto-created default graph"}
        if self.rest is None:
            self.new_rest()
        t_start = CLOCK()
        for i in range(1, self.n):
            name = graph_name(i)
            t0 = CLOCK()
            try:
                self.rest.create_graph(name, max_size_gb=self.args.max_size_gb)
            except Exception as exc:
                # Record the failure point precisely: this is a finding.
                return {
                    "created": len(self.created),
                    "requested": self.n - 1,
                    "failed_at_graph_index": i,
                    "graphs_present_at_failure": i,
                    "error": "%s: %s" % (type(exc).__name__, exc),
                    "peak_observed": self.server.samples()["peaks"],
                    "latency": latency_summary(self.create_latencies),
                    "buckets": {k: latency_summary(v)
                                for k, v in sorted(self.create_buckets.items())},
                    "total_seconds": round(CLOCK() - t_start, 3),
                }
            dt = CLOCK() - t0
            self.create_latencies.append(dt)
            self.created.append(name)
            self.create_buckets.setdefault(bucket_label(i), []).append(dt)
        return {
            "created": len(self.created),
            "requested": self.n - 1,
            "total_seconds": round(CLOCK() - t_start, 3),
            "latency": latency_summary(self.create_latencies),
            "buckets": {k: latency_summary(v) for k, v in sorted(self.create_buckets.items())},
        }

    def delete_graphs(self):
        if not self.created:
            return {"deleted": 0}
        if self.rest is None:
            self.new_rest()
        remaining = self.n
        t_start = CLOCK()
        deleted = 0
        for name in list(self.created):
            t0 = CLOCK()
            try:
                self.rest.delete_graph(name)
            except Exception as exc:
                return {
                    "deleted": deleted,
                    "requested": len(self.created),
                    "failed_on": name,
                    "graphs_remaining": remaining,
                    "error": "%s: %s" % (type(exc).__name__, exc),
                    "peak_observed": self.server.samples()["peaks"] if self.live() else None,
                    "latency": latency_summary(self.delete_latencies),
                    "buckets": {k: latency_summary(v)
                                for k, v in sorted(self.delete_buckets.items())},
                    "total_seconds": round(CLOCK() - t_start, 3),
                }
            dt = CLOCK() - t0
            self.delete_latencies.append(dt)
            self.delete_buckets.setdefault(bucket_label(remaining), []).append(dt)
            remaining -= 1
            deleted += 1
        return {
            "deleted": deleted,
            "requested": len(self.created),
            "total_seconds": round(CLOCK() - t_start, 3),
            "latency": latency_summary(self.delete_latencies),
            "buckets": {k: latency_summary(v) for k, v in sorted(self.delete_buckets.items())},
        }

    # -- queries ------------------------------------------------------------

    def first_query(self):
        r = self.new_rest()
        out = {}
        t0 = CLOCK()
        r.cypher(READ_QUERY, "default")
        out["default_seconds"] = round(CLOCK() - t0, 6)
        if self.created:
            t0 = CLOCK()
            r.cypher(READ_QUERY, self.created[-1])
            out["last_graph_seconds"] = round(CLOCK() - t0, 6)
            out["last_graph"] = self.created[-1]
        return out

    def repeated_query(self):
        r = self.new_rest()
        k = self.args.repeated_queries
        names = ["default"] + self.created
        lat = []
        # warm up so we measure steady state, not the cold path
        for _ in range(min(20, k)):
            r.cypher(READ_QUERY, "default")
        for i in range(k):
            g = names[i % len(names)] if len(names) > 1 else "default"
            t0 = CLOCK()
            r.cypher(READ_QUERY, g)
            lat.append(CLOCK() - t0)
        return {
            "queries": k,
            "graphs_spread_over": min(len(names), k),
            "latency": latency_summary(lat),
            "qps": round(k / sum(lat), 1) if sum(lat) > 0 else None,
        }

    def ensure_bench_schema(self):
        """Declare the label used by the write-throughput query.

        TuGraph requires a label to be declared before vertices of that label
        can be created (see `db.createVertexLabel`), so the write benchmark
        cannot use an undeclared label. Idempotent: an existing label raises,
        which we ignore.
        """
        r = self.new_rest()
        stmts = [
            "CALL db.createVertexLabel('phase0bench', 'v', 'v', 'INT64', false)",
        ]
        for s in stmts:
            try:
                r.cypher(s, "default")
            except Exception:
                pass

    def throughput(self):
        self.ensure_bench_schema()
        secs = self.args.throughput_seconds
        nthreads = self.args.throughput_threads
        read_counts = [0] * nthreads
        write_counts = [0] * nthreads
        errors = [0] * nthreads
        stop_at = CLOCK() + secs

        def worker(idx, write):
            client = restclient.RestClient(
                "127.0.0.1", self.server.http_port, timeout=self.args.http_timeout)
            # 'v' is the label's unique primary field, so each writer thread
            # needs a disjoint value range or the inserts would conflict.
            counter = idx * WRITE_KEY_STRIDE
            try:
                while CLOCK() < stop_at:
                    try:
                        if write:
                            counter += 1
                            client.cypher(WRITE_QUERY_FMT % counter, "default")
                            write_counts[idx] += 1
                        else:
                            client.cypher(READ_QUERY, "default")
                            read_counts[idx] += 1
                    except Exception:
                        errors[idx] += 1
                        if errors[idx] > 100:
                            break
                        time.sleep(0.01)
            finally:
                client.close()

        threads = []
        for i in range(nthreads):
            threads.append(threading.Thread(target=worker, args=(i, False)))
        wthreads = []
        for i in range(max(1, nthreads // 2)):
            wthreads.append(threading.Thread(target=worker, args=(i, True)))

        for t in threads + wthreads:
            t.start()
        for t in threads + wthreads:
            t.join()

        reads = sum(read_counts)
        writes = sum(write_counts)
        return {
            "seconds": secs,
            "read_threads": nthreads,
            "write_threads": max(1, nthreads // 2),
            "reads": reads,
            "writes": writes,
            "read_tps": round(reads / secs, 1),
            "write_tps": round(writes / secs, 1),
            "errors": sum(errors),
        }

    # -- snapshot / restore -------------------------------------------------

    def snapshot(self):
        r = self.new_rest()
        if os.path.isdir(self.snapshot_dir):
            shutil.rmtree(self.snapshot_dir)
        os.makedirs(self.snapshot_dir)
        # The server writes into its configured snapshot_dir, not ours; use the
        # Cypher procedure and then locate the produced tree.
        t0 = CLOCK()
        try:
            r.cypher("CALL dbms.takeSnapshot()", "default")
        except Exception as exc:
            return {"error": "%s: %s" % (type(exc).__name__, exc)}
        secs = CLOCK() - t0
        snap_root = os.path.join(self.db_dir, "snapshot")
        info = {"seconds": round(secs, 3), "snapshot_root": snap_root}
        if os.path.isdir(snap_root):
            subs = sorted(os.listdir(snap_root))
            info["snapshots"] = subs
            if subs:
                latest = os.path.join(snap_root, subs[-1])
                info["latest"] = latest
                info["bytes"] = procmetrics.dir_size_bytes(latest)
                info["files"] = procmetrics.count_files(latest)
        return info

    def restore(self):
        """Copy a snapshot tree into a fresh dir and start a server on it."""
        snap_root = os.path.join(self.db_dir, "snapshot")
        if not os.path.isdir(snap_root) or not os.listdir(snap_root):
            return {"error": "no snapshot available to restore"}
        latest = os.path.join(snap_root, sorted(os.listdir(snap_root))[-1])
        if os.path.isdir(self.restore_dir):
            shutil.rmtree(self.restore_dir)
        t0 = CLOCK()
        shutil.copytree(latest, self.restore_dir)
        copy_seconds = CLOCK() - t0

        srv = serverctl.Server(
            binary=self.args.binary, db_dir=self.restore_dir,
            config_path=self.args.config, extra_args=self.args.server_arg,
            log_path=os.path.join(self.args.data_dir, "logs",
                                  "restore_%d.log" % self.n),
            startup_timeout=self.args.startup_timeout,
            sampler_interval=self.args.sample_interval)
        startup_seconds = srv.start()
        restclient.wait_until_ready("127.0.0.1", srv.http_port,
                                    timeout=self.args.http_timeout)
        verified = None
        graphs = None
        err = None
        try:
            rc = restclient.RestClient("127.0.0.1", srv.http_port,
                                       timeout=self.args.http_timeout)
            graphs = rc.graph_names()
            rc.cypher(READ_QUERY, "default")
            verified = True
            rc.close()
        except Exception as exc:
            err = "%s: %s" % (type(exc).__name__, exc)
            verified = False
        finally:
            srv.stop()
        out = {
            "copy_seconds": round(copy_seconds, 3),
            "startup_seconds": round(startup_seconds, 3),
            "total_seconds": round(copy_seconds + startup_seconds, 3),
            "graphs_found": len(graphs) if isinstance(graphs, list) else None,
            "verified": verified,
        }
        if err:
            out["verify_error"] = err
        return out

    # -- metrics ------------------------------------------------------------

    def steady_metrics(self):
        """Idle-but-live resource usage after startup settles.

        `last` is the most recent sample (the steady-state figure); the `_peak`
        fields are the maxima observed so far in the run. `maps` is only sampled
        periodically because reading /proc/<pid>/maps is O(mappings), so it can
        be absent from the most recent sample -- fall back to the peak.
        """
        if not self.live():
            return {"error": "server not running"}
        time.sleep(self.args.settle_seconds)
        s = self.server.samples()
        last, peaks = s["last"], s["peaks"]
        return {
            "rss_kb": last["rss_kb"],
            "vmsize_kb": last["vmsize_kb"],
            "threads": last["threads"],
            "fds": last["fds"],
            "maps": last.get("maps") if last.get("maps") is not None
                    else peaks.get("maps"),
            "rss_peak_kb": peaks.get("rss_kb"),
            "threads_peak": peaks.get("threads"),
            "fds_peak": peaks.get("fds"),
            "maps_peak": peaks.get("maps"),
            "vmsize_peak_kb": peaks.get("vmsize_kb"),
            "peaks_during_run": peaks,
        }

    def storage_metrics(self):
        """On-disk footprint, separating the shared meta store from graphs.

        `.meta` holds the server-wide graph registry and ACL, and `snapshot`
        is the snapshot output tree. Neither is a graph, so both are excluded
        from the per-graph figure.
        """
        total = procmetrics.dir_size_bytes(self.db_dir)
        files = procmetrics.count_files(self.db_dir)
        graph_dirs = []
        for name in sorted(os.listdir(self.db_dir)):
            p = os.path.join(self.db_dir, name)
            if not os.path.isdir(p):
                continue
            # Graph directories are 16-uppercase-hex names. Everything else
            # that lives in db_dir is not a graph: `.meta` (server registry and
            # ACL), `snapshot`, `raftlog`, and `_audit_log_` / `_import_tmp`
            # which the server creates lazily.
            if name.startswith(".") or name.startswith("_"):
                continue
            if name in ("snapshot", "raftlog", "binlog"):
                continue
            graph_dirs.append(p)
        graph_bytes = sum(procmetrics.dir_size_bytes(p) for p in graph_dirs)
        meta_dir = os.path.join(self.db_dir, ".meta")
        return {
            "dir_bytes": total,
            "graph_bytes": graph_bytes,
            "meta_bytes": (procmetrics.dir_size_bytes(meta_dir)
                           if os.path.isdir(meta_dir) else None),
            "files": files,
            "graph_dirs": len(graph_dirs),
            "per_graph_bytes": (round(graph_bytes / float(len(graph_dirs)), 1)
                                if graph_dirs else None),
        }

    # -- drive --------------------------------------------------------------

    def run_cycle(self):
        rec = self.rec
        run = self.run
        rec.doc["runs"][str(self.n)] = run

        if self.args.reuse_db and os.path.isdir(self.db_dir):
            eprint("  [%d] reusing existing db_dir" % self.n)
        else:
            if os.path.isdir(self.db_dir):
                shutil.rmtree(self.db_dir)
            os.makedirs(self.db_dir)

        phases = self.args.phases

        if _phase(phases, "start", "startup"):
            phase(rec, run, "startup", self.do_startup)
            if not self.live():
                eprint("  [%d] server failed to start; skipping remaining phases" % self.n)
                return run
            phase(rec, run, "startup_metrics", lambda: {
                "sample": procmetrics.sample(self.server.proc.pid, with_maps=True),
                "peaks": self.server.samples()["peaks"]})

        if "create" in phases:
            phase(rec, run, "create", self.create_graphs)

        if "metrics" in phases:
            phase(rec, run, "steady_metrics", self.steady_metrics)
            phase(rec, run, "storage", self.storage_metrics)

        if "query" in phases:
            phase(rec, run, "first_query", self.first_query)
            phase(rec, run, "repeated_query", self.repeated_query)

        if "throughput" in phases:
            phase(rec, run, "throughput", self.throughput)

        if "snapshot" in phases:
            phase(rec, run, "snapshot", self.snapshot)

        if "restart" in phases:
            snap = None
            if self.live():
                phase(rec, run, "shutdown", lambda: {
                    "seconds": round(self.server.stop(), 4)})
            # restart with N graphs on disk: the key scaling number
            phase(rec, run, "restart", self.do_startup)

        if "restore" in phases:
            phase(rec, run, "restore", self.restore)

        if "delete" in phases:
            if not self.live():
                eprint("  [%d] server down before delete phase; restarting" % self.n)
                phase(rec, run, "restart_for_delete", self.do_startup)
            phase(rec, run, "delete", self.delete_graphs)

        if "stop" in phases and self.live():
            phase(rec, run, "final_shutdown", lambda: {
                "seconds": round(self.server.stop(), 4)})

        run["final_peaks"] = {
            "startup": run.get("startup_metrics", {}).get("peaks"),
            "steady": run.get("steady_metrics", {}).get("peaks_during_run"),
        }
        rec.flush()
        return run

    def cleanup(self):
        if self.server is not None and self.live():
            self.server.kill()
        if self.rest is not None:
            self.rest.close()


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def parse_args(argv):
    ap = argparse.ArgumentParser(
        description="Phase 0 TuGraph graph-count scaling benchmark")
    ap.add_argument("--graphs", type=int, nargs="+", default=DEFAULT_GRAPHS,
                    help="graph counts to measure (default: 1 100 1000 4000)")
    ap.add_argument("--binary", default=None,
                    help="path to lgraph_server (default: build/output/lgraph_server)")
    ap.add_argument("--config", default=None,
                    help="server config JSON (default: build/output/lgraph_standalone.json)")
    ap.add_argument("--data-dir", default=os.environ.get("PHASE0_DATA_DIR", "/data"),
                    help="scratch directory for databases (default: $PHASE0_DATA_DIR or /data)")
    ap.add_argument("--out", default=None,
                    help="result JSON path (default: benchmark/scaling/results/baseline.json)")
    ap.add_argument("--max-size-gb", type=int, default=1,
                    help="per-graph max_size_GB (default 1; the default 4 TiB would "
                         "exhaust virtual address space)")
    ap.add_argument("--repeated-queries", type=int, default=1000)
    ap.add_argument("--throughput-seconds", type=float, default=10.0)
    ap.add_argument("--throughput-threads", type=int, default=4)
    ap.add_argument("--settle-seconds", type=float, default=3.0)
    ap.add_argument("--sample-interval", type=float, default=0.5)
    ap.add_argument("--startup-timeout", type=float, default=1800.0)
    ap.add_argument("--http-timeout", type=float, default=600.0)
    ap.add_argument("--server-arg", action="append", default=[],
                    help="extra lgraph_server argument (repeatable)")
    ap.add_argument("--phases", nargs="+", default=[
        "start", "create", "metrics", "query", "throughput",
        "snapshot", "restart", "restore", "delete", "stop"],
        help="subset of phases to run")
    ap.add_argument("--reuse-db", action="store_true",
                    help="reuse an existing db_dir instead of recreating it")
    ap.add_argument("--keep", action="store_true",
                    help="keep db/snapshot dirs after the run")
    ap.add_argument("--quick", action="store_true",
                    help="fast smoke configuration (few queries, short throughput)")
    args = ap.parse_args(argv)

    repo_root = os.path.abspath(os.path.join(HERE, "..", ".."))
    if args.binary is None:
        args.binary = os.path.join(repo_root, "build", "output", "lgraph_server")
    if args.config is None:
        args.config = os.path.join(repo_root, "build", "output", "lgraph_standalone.json")
    if args.out is None:
        args.out = os.path.join(HERE, "results", "baseline.json")
    args.repo_root = repo_root

    if args.quick:
        args.repeated_queries = min(args.repeated_queries, 100)
        args.throughput_seconds = min(args.throughput_seconds, 3.0)
        args.settle_seconds = min(args.settle_seconds, 1.0)
    return args


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])

    if not os.path.isfile(args.binary):
        eprint("ERROR: server binary not found: %s" % args.binary)
        eprint("Build it first with ci/phase0/build.sh")
        return 2
    if not os.path.isfile(args.config):
        eprint("ERROR: server config not found: %s" % args.config)
        return 2
    os.makedirs(args.data_dir, exist_ok=True)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    os.makedirs(os.path.join(args.data_dir, "logs"), exist_ok=True)

    rec = Recorder(args.out)
    rec.set("config", {
        "graphs": args.graphs,
        "max_size_gb": args.max_size_gb,
        "repeated_queries": args.repeated_queries,
        "throughput_seconds": args.throughput_seconds,
        "throughput_threads": args.throughput_threads,
        "settle_seconds": args.settle_seconds,
        "sample_interval": args.sample_interval,
        "phases": args.phases,
        "server_extra_args": args.server_arg,
        "binary": args.binary,
        "config_file": args.config,
    })
    rec.set("env", envprobe.capture(args.data_dir, args.repo_root))
    rec.flush()

    eprint("phase0 bench: graphs=%s data_dir=%s out=%s" % (args.graphs, args.data_dir, args.out))

    for n in args.graphs:
        eprint("=== graph count %d ===" % n)
        cyc = Cycle(args, rec, n)
        try:
            cyc.run_cycle()
        except Exception as exc:  # noqa: BLE001
            cyc.run["fatal"] = "%s: %s\n%s" % (type(exc).__name__, exc, _tb_tail())
            rec.flush()
            eprint("  [%d] FATAL: %s" % (n, exc))
        finally:
            cyc.cleanup()
            if not args.keep:
                for d in (cyc.db_dir, cyc.restore_dir):
                    if os.path.isdir(d):
                        shutil.rmtree(d, ignore_errors=True)

    rec.doc["finished_at_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    rec.flush()
    eprint("phase0 bench: wrote %s" % args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
