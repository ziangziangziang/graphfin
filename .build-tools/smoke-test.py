#!/usr/bin/env python3
"""Smoke-test the freshly built lgraph_server over the REST API.

Start the server on a scratch directory, log in, run a Cypher round-trip and
shut it down again.  Stdlib only.
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request

DEFAULT_USER = "admin"
DEFAULT_PASSWORD = "73@TuGraph"


def http(url, body=None, token=None, method=None, timeout=60):
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers=headers,
                                 method=method or ("POST" if data else "GET"))
    with urllib.request.urlopen(req, timeout=timeout) as r:
        raw = r.read().decode()
        try:
            return r.status, json.loads(raw)
        except json.JSONDecodeError:
            return r.status, raw


def wait_up(base, deadline_s=120):
    t0 = time.time()
    last = None
    while time.time() - t0 < deadline_s:
        try:
            st, _ = http(base + "/login", {"user": DEFAULT_USER,
                                           "password": DEFAULT_PASSWORD}, timeout=5)
            if st == 200:
                return True
        except Exception as e:  # noqa: BLE001
            last = e
        time.sleep(2)
    print("server did not come up: %s" % last)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default="/home/pdfexe/graphfin/build/output/lgraph_server")
    ap.add_argument("--config", default="/home/pdfexe/graphfin/src/server/lgraph_standalone.json")
    ap.add_argument("--datadir", default="/tmp/graphfin_smoke_db")
    ap.add_argument("--port", type=int, default=7073)
    ap.add_argument("--rpc-port", type=int, default=9093)
    ap.add_argument("--host", default="127.0.0.1")
    args = ap.parse_args()

    if not os.path.exists(args.binary):
        print("missing binary: %s" % args.binary)
        return 1

    shutil.rmtree(args.datadir, ignore_errors=True)
    os.makedirs(args.datadir, exist_ok=True)

    cmd = [args.binary, "-c", args.config, "--directory", args.datadir,
           "--host", args.host, "--port", str(args.port),
           "--rpc_port", str(args.rpc_port)]
    print("starting:", " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=os.path.dirname(args.binary),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    base = "http://%s:%d" % (args.host, args.port)
    ok = False
    try:
        if not wait_up(base):
            return 1
        ok = True
        st, body = http(base + "/login", {"user": DEFAULT_USER,
                                          "password": DEFAULT_PASSWORD})
        token = body["jwt"]
        print("login ok, token len=%d" % len(token))

        st, body = http(base + "/cypher",
                        {"graph": "default", "script": "RETURN 1 AS one", "parameters": {}},
                        token=token)
        print("cypher RETURN ->", st, json.dumps(body)[:300])

        # Cypher has no schema DDL; the label has to exist before a vertex can
        # reference it, so it is created through the graph-api endpoint.
        st, body = http(base + "/db/default/label",
                        {"name": "Smoke", "is_vertex": True, "primary": "name",
                         "fields": [{"name": "name", "type": "STRING"},
                                    {"name": "n", "type": "INT32"}]},
                        token=token)
        print("create label  ->", st, json.dumps(body)[:300])

        st, body = http(base + "/cypher",
                        {"graph": "default",
                         "script": "CREATE (:Smoke {name:'graphfin', n:1}) RETURN 1 AS created",
                         "parameters": {}},
                        token=token)
        print("cypher CREATE ->", st, json.dumps(body)[:300])

        st, body = http(base + "/cypher",
                        {"graph": "default",
                         "script": "MATCH (s:Smoke) RETURN count(s) AS c",
                         "parameters": {}},
                        token=token)
        print("cypher MATCH  ->", st, json.dumps(body)[:300])
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
        out = proc.stdout.read().decode(errors="replace") if proc.stdout else ""
        tail = "\n".join(out.splitlines()[-25:])
        print("--- server log tail ---")
        print(tail)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
