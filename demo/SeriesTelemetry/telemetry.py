#!/usr/bin/env python3
"""Equipment-telemetry example: declare -> ingest -> query -> export.

Uses only the public REST surface (stdlib only, no extra packages):
start a server first, e.g.
  ./lgraph_server -c lgraph_standalone.json --directory ./demodb \
      --host 127.0.0.1 --port 7073 --rpc_port 9093
then run:
  python3 telemetry.py --port 7073

The same code path serves the financial example (demo/SeriesFinancial);
only the fixture differs.

Fixture notes: `site` is an ordinary indexed STRING property used for
grouping (`MATCH (d:Sensor {site:'hall-0'})`); nothing in the engine is
telemetry-specific. See docs/architecture/09-series-client-contracts.md
for the wire shapes this example relies on.
"""

import argparse
import datetime
import json
import urllib.request

BASE_TS = int(datetime.datetime(2024, 1, 2).timestamp() * 1_000_000)


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
    rest.cypher("CALL db.createVertexLabel('Sensor', 'id', 'id', 'STRING', false, "
                "'site', 'STRING', true)")
    rest.cypher("CALL db.createSeriesField('Sensor', 'readings', "
                "[{name:'temp_c', type:'DOUBLE'}, {name:'rh_pct', type:'DOUBLE'}, "
                "{name:'flags', type:'INT64'}], {bucket_max_points:1000}) "
                "YIELD field RETURN field")


def ingest(rest, n_sensors=4, n_points=48):
    for s in range(n_sensors):
        rest.cypher("CREATE (d:Sensor {id:'sensor-%02d', site:'hall-%d'})" % (s, s % 2))
        for p in range(n_points):
            ts = BASE_TS + p * 3600 * 1_000_000
            rest.cypher(
                "MATCH (d:Sensor {id:'sensor-%02d'}) CALL series.append(d, 'readings', "
                "{ts: %d, temp_c: %f, rh_pct: %f, flags: %d}) "
                "YIELD written RETURN written" % (s, ts, 20.0 + s + p * 0.05,
                                                  40.0 + p * 0.1, p % 3))


def query(rest):
    n = rest.cypher("MATCH (d:Sensor {id:'sensor-00'}) "
                    "RETURN series.count(d, 'readings') AS n")[0]["n"]
    latest = rest.cypher("MATCH (d:Sensor {id:'sensor-00'}) "
                         "RETURN series.latest(d, 'readings') AS p")[0]["p"]
    mean = rest.cypher("MATCH (d:Sensor {id:'sensor-00'}) "
                       "RETURN series.mean(d, 'readings', 'temp_c') AS m")[0]["m"]
    return n, latest, mean


def export(rest, path):
    rows = rest.cypher(
        "MATCH (d:Sensor {id:'sensor-00'}) RETURN series.range(d, 'readings', "
        "%d, %d) AS pts" % (BASE_TS, BASE_TS + 48 * 3600 * 1_000_000))[0]["pts"]
    points = rows if isinstance(rows, list) else json.loads(rows)
    with open(path, "w") as f:
        for p in points:
            f.write(json.dumps(p) + "\n")
    return len(points)


def main():
    ap = argparse.ArgumentParser(description="Series telemetry example")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7073)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="73@TuGraph")
    ap.add_argument("--out", default="/tmp/telemetry_export.jsonl")
    args = ap.parse_args()
    rest = Rest(args.host, args.port, args.user, args.password)
    declare(rest)
    ingest(rest)
    n, latest, mean = query(rest)
    count = export(rest, args.out)
    print("count=%s latest=%s mean_temp=%.4f exported=%d rows to %s"
          % (n, latest, mean, count, args.out))


if __name__ == "__main__":
    main()
