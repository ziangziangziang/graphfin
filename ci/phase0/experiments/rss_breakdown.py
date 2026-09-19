#!/usr/bin/env python3
"""Phase 1 investigation: where does the ~1.5 MiB per-graph RSS go?

Starts an lgraph_server, creates graphs in stages, and after each stage dumps
/proc/<pid>/smaps aggregated by mapping *kind*:

  data.mdb   LMDB database file, one per graph (mmap'd, sparse on disk)
  lock.mdb   LMDB reader-slot table, one per graph
  heap       [heap] and anonymous mappings (malloc'd structures)
  stack      thread stacks (one per graph: the LMDB validator thread)
  other      named files that are not LMDB (shared libraries etc.)

The per-stage deltas give the marginal cost of one graph, split by kind, which
is what tells us whether the memory is LMDB page cache (movable to disk) or
process heap (not).

Standard library only (Python 3.6 compatible).

Usage (inside the pinned compile image, from build/output):
    python3 rss_breakdown.py [--graphs 800] [--steps 4] [--json out.json]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

READY_MARKER = "Server started."

SMAPS_KEYS = ("Rss", "Pss", "Private_Clean", "Private_Dirty", "Shared_Clean",
              "Shared_Dirty", "Swap")


def parse_smaps(pid):
    """Aggregate /proc/<pid>/smaps into {kind: {key: bytes}}.

    A mapping whose pathname ends in data.mdb / lock.mdb is LMDB; everything
    else is bucketed by whether it is anonymous, a stack, or a named file.
    """
    agg = {}
    path = "/proc/%d/smaps" % pid
    try:
        f = open(path, "r")
    except (IOError, OSError):
        return agg

    kind = None
    with f:
        for line in f:
            # A new mapping starts with an address range.
            if re.match(r"^[0-9a-f]+-[0-9a-f]+ ", line):
                m = re.search(r"\s(\S*)$", line.rstrip("\n"))
                name = m.group(1) if m else ""
                if name.endswith("/data.mdb"):
                    kind = "data.mdb"
                elif name.endswith("/lock.mdb"):
                    kind = "lock.mdb"
                elif name.startswith("[stack"):
                    kind = "stack"
                elif name in ("[heap]", "[anon]"):
                    kind = "heap"
                elif name == "":
                    kind = "heap"      # anonymous mapping
                elif name.startswith("["):
                    kind = "other"
                else:
                    kind = "other"
                continue
            m = re.match(r"^([A-Za-z_]+):\s+(\d+)\s+kB", line)
            if m and m.group(1) in SMAPS_KEYS:
                key = m.group(1)
                kb = int(m.group(2))
                slot = agg.setdefault(kind, {})
                slot[key] = slot.get(key, 0) + kb
    return agg


def sample(pid):
    s = parse_smaps(pid)
    total_kb = sum(v.get("Rss", 0) for v in s.values())
    s["total_kb"] = total_kb
    return s


def start_server(binary, db_dir, log_path, http_port, rpc_port, bolt_port,
                 extra_args=None):
    os.makedirs(db_dir, exist_ok=True)
    logf = open(log_path, "w")
    cmd = [binary, "-c", "lgraph_standalone.json",
           "--directory", db_dir, "--host", "127.0.0.1",
           "--port", str(http_port), "--rpc_port", str(rpc_port),
           "--bolt_port", str(bolt_port), "--verbose", "1"]
    cmd += list(extra_args or [])
    proc = subprocess.Popen(
        cmd,
        stdout=logf, stderr=subprocess.STDOUT, close_fds=True,
        cwd=os.path.dirname(os.path.abspath(binary)))
    deadline = time.time() + 300
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError("server exited early")
        with open(log_path) as f:
            if READY_MARKER in f.read():
                return proc
        time.sleep(0.05)
    proc.kill()
    raise RuntimeError("server did not become ready")


def wait_rest(port, timeout=120):
    import socket
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=2)
            s.close()
            return
        except (socket.error, OSError):
            time.sleep(0.2)
    raise RuntimeError("REST port never came up")


class Client(object):
    """Minimal REST client for graph creation (stdlib http.client)."""

    def __init__(self, port, user="admin", password="73@TuGraph"):
        import http.client
        self.conn = http.client.HTTPConnection("127.0.0.1", port, timeout=600)
        self.user = user
        self.password = password
        self.token = None

    def request(self, method, path, body=None, auth=True):
        headers = {"Accept": "application/json"}
        payload = None
        if body is not None:
            payload = json.dumps(body)
            headers["Content-Type"] = "application/json"
        if auth and self.token:
            headers["Authorization"] = "Bearer " + self.token
        self.conn.request(method, path, body=payload, headers=headers)
        resp = self.conn.getresponse()
        raw = resp.read()
        try:
            return resp.status, json.loads(raw.decode("utf-8")) if raw else None
        except ValueError:
            return resp.status, raw.decode("utf-8", "replace")

    def login(self):
        _, body = self.request("POST", "/login",
                               {"user": self.user, "password": self.password},
                               auth=False)
        self.token = body["jwt"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="./lgraph_server")
    ap.add_argument("--graphs", type=int, default=800)
    ap.add_argument("--steps", type=int, default=4)
    ap.add_argument("--db-dir", default="/tmp/rss_probe")
    ap.add_argument("--log", default="/tmp/rss_probe.log")
    ap.add_argument("--json", default="")
    ap.add_argument("--server-arg", action="append", default=[],
                    help="extra lgraph_server argument (repeatable)")
    args = ap.parse_args()

    for d in (args.db_dir, args.log):
        if os.path.isdir(d):
            import shutil
            shutil.rmtree(d, ignore_errors=True)
        elif os.path.isfile(d):
            os.remove(d)

    proc = start_server(args.binary, args.db_dir, args.log,
                        17400, 19400, 17401, args.server_arg)
    pid = proc.pid
    wait_rest(17400)
    c = Client(17400)
    c.login()

    stages = [0]
    per = max(1, args.graphs // args.steps)
    for i in range(1, args.steps + 1):
        stages.append(min(args.graphs, per * i))
    stages = sorted(set(stages))

    samples = []
    created = 0
    for stage in stages:
        while created < stage:
            created += 1
            status, body = c.request(
                "POST", "/db",
                {"name": "g%05d" % created,
                 "config": {"max_size_GB": 1, "description": "rss probe"}})
            if status != 200:
                raise RuntimeError("create failed at graph %d: %s" % (created, body))
        # give the server a moment to settle
        time.sleep(3)
        s = sample(pid)
        samples.append({"graphs": stage, "smaps": s})
        print("stage graphs=%-5d total_rss=%.1f MiB" % (
            stage, s["total_kb"] / 1024.0))

    # ---- report marginal cost per graph, by kind ----
    print()
    kinds = ("data.mdb", "lock.mdb", "heap", "stack", "other")
    print("%-8s %10s %10s %10s %10s %10s %12s" %
          (("graphs",) + kinds + ("total MiB",)))
    base = samples[0]
    base_smaps = base["smaps"]
    for entry in samples:
        smaps = entry["smaps"]
        row = []
        for k in kinds:
            have = smaps.get(k, {}).get("Rss", 0)
            basev = base_smaps.get(k, {}).get("Rss", 0)
            row.append((have - basev) / 1024.0)   # MiB added since stage 0
        total_mib = smaps["total_kb"] / 1024.0
        print("%-8d" % entry["graphs"] +
              "".join("%10.2f" % v for v in row) +
              "%12.2f" % total_mib)
    print("(values are MiB added relative to the 1-graph baseline)")

    # ---- absolute per-kind snapshot at the final stage ----
    print()
    print("%-10s %10s %10s %12s %12s" % ("kind", "Rss MiB", "PSS MiB",
                                        "PrivDirty MiB", "PrivClean MiB"))
    final = samples[-1]["smaps"]
    for k in kinds + ("other",):
        v = final.get(k)
        if not v:
            continue
        print("%-10s %10.2f %10.2f %12.2f %12.2f" % (
            k, v.get("Rss", 0) / 1024.0, v.get("Pss", 0) / 1024.0,
            v.get("Private_Dirty", 0) / 1024.0,
            v.get("Private_Clean", 0) / 1024.0))

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"stages": samples,
                       "graphs": args.graphs, "steps": args.steps}, f,
                      indent=2, sort_keys=True)
        print("wrote %s" % args.json)

    proc.terminate()
    proc.wait(timeout=60)


if __name__ == "__main__":
    main()
