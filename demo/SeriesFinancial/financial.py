#!/usr/bin/env python3
"""Financial OHLCV example: declare -> ingest -> query -> export.

Same code path as demo/SeriesTelemetry; only the fixture differs (symbols
instead of sensors, open/high/low/close/volume instead of temp/humidity).
Start a server first, e.g.
  ./lgraph_server -c lgraph_standalone.json --directory ./demodb \
      --host 127.0.0.1 --port 7073 --rpc_port 9093
then run:
  python3 financial.py --port 7073
"""

import argparse
import datetime
import json
import urllib.request

BASE_TS = int(datetime.datetime(2024, 1, 2).timestamp() * 1_000_000)
DAY_US = 86400 * 1_000_000
SYMBOLS = ("ACME", "GLOBEX", "INITECH")


class Rest:
    def __init__(self, host, port, user, password):
        self.base = "http://%s:%d/" % (host, port)
        token = self._post("login", {"user": user, "password": password})["jwt"]
        self.headers = {"Content-Type": "application/json",
                        "Authorization": "Bearer " + token}

    def _post(self, path, body):
        req = urllib.request.Request(
            self.base + path, data=json.dumps(body).encode(),
            headers=self.headers if hasattr(self, "headers") else
            {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=120) as r:
            return json.loads(r.read().decode())

    def cypher(self, script, graph="default"):
        body = self._post("cypher", {"script": script, "graph": graph})
        names = [c["name"] for c in body.get("header", [])]
        return [dict(zip(names, row)) for row in body.get("result", [])]


def declare(rest):
    rest.cypher("CALL db.createVertexLabel('Company', 'id', 'id', 'STRING', false, "
                "'sector', 'STRING', true)")
    rest.cypher("CALL db.createSeriesField('Company', 'prices', "
                "[{name:'open', type:'DOUBLE'}, {name:'high', type:'DOUBLE'}, "
                "{name:'low', type:'DOUBLE'}, {name:'close', type:'DOUBLE'}, "
                "{name:'volume', type:'INT64'}], {bucket_max_points:1000}) "
                "YIELD field RETURN field")


def ingest(rest, n_days=30):
    for s, sym in enumerate(SYMBOLS):
        rest.cypher("CREATE (c:Company {id:'%s', sector:'tech'})" % sym)
        for d in range(n_days):
            ts = BASE_TS + d * DAY_US
            o = 100.0 + s * 10 + d * 0.5
            rest.cypher(
                "MATCH (c:Company {id:'%s'}) CALL series.append(c, 'prices', "
                "{ts: %d, open: %f, high: %f, low: %f, close: %f, volume: %d}) "
                "YIELD written RETURN written"
                % (sym, ts, o, o + 1.0, o - 0.5, o + 0.25, 1000 + d * 10))


def query(rest):
    n = rest.cypher("MATCH (c:Company {id:'ACME'}) "
                    "RETURN series.count(c, 'prices') AS n")[0]["n"]
    latest = rest.cypher("MATCH (c:Company {id:'ACME'}) "
                         "RETURN series.latest(c, 'prices') AS p")[0]["p"]
    if isinstance(latest, str):
        latest = json.loads(latest)
    mean_vol = rest.cypher("MATCH (c:Company {id:'ACME'}) "
                           "RETURN series.mean(c, 'prices', 'volume') AS m")[0]["m"]
    return n, latest, mean_vol


def export(rest, path):
    rows = rest.cypher(
        "MATCH (c:Company {id:'ACME'}) RETURN series.range(c, 'prices', %d, %d) AS pts"
        % (BASE_TS, BASE_TS + 30 * DAY_US))[0]["pts"]
    points = rows if isinstance(rows, list) else json.loads(rows)
    with open(path, "w") as f:
        for p in points:
            f.write(json.dumps(p) + "\n")
    return len(points)


def main():
    ap = argparse.ArgumentParser(description="Series financial example")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7073)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="73@TuGraph")
    ap.add_argument("--out", default="/tmp/financial_export.jsonl")
    args = ap.parse_args()
    rest = Rest(args.host, args.port, args.user, args.password)
    declare(rest)
    ingest(rest)
    n, latest, mean_vol = query(rest)
    count = export(rest, args.out)
    print("count=%s latest=%s mean_volume=%s exported=%d rows to %s"
          % (n, latest, mean_vol, count, args.out))


if __name__ == "__main__":
    main()
