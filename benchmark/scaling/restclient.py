"""Minimal REST client for the TuGraph C++ restful server.

Deliberately hand-written against the verified server behaviour rather than
reusing src/client/python/TuGraphClient/TuGraphRestClient.py, which targets a
different (older) API shape and would not work against this server:

  * login   POST /login  {"user": "admin", "password": "..."}
            -> {"jwt": "<token>", "default_password": bool, "isAdmin": bool}
            (src/restful/server/rest_server.cpp:1518-1545)
  * auth    header "Authorization: Bearer <token>"  (strict prefix check,
            src/restful/server/rest_server.cpp:569-584)
  * create  POST /db     {"name": ..., "config": {"max_size_GB": N, ...}}
            (src/restful/server/rest_server.cpp:2506-2532)
  * delete  DELETE /db/{name}                       (:2675-2696)
  * list    GET /db                                 (:1423-1438)
  * query   POST /cypher {"script": ..., "graph": ..., "timeout": N}
            (src/restful/server/rest_server.cpp:1684-1731)
  * there is no {errorCode, data} envelope; successful responses are the raw
    JSON body (RestServer::RespondSuccess, :680-694)

Standard library only (Python 3.6 compatible).
"""

import http.client
import json
import socket
import time

CLOCK = time.perf_counter


class RestError(RuntimeError):
    def __init__(self, status, body, path):
        RuntimeError.__init__(
            self, "HTTP %s from %s: %s" % (status, path, str(body)[:400]))
        self.status = status
        self.body = body
        self.path = path


class RestClient(object):
    def __init__(self, host="127.0.0.1", port=7070, user="admin",
                 password="73@TuGraph", timeout=600.0, auto_login=True):
        self.host = host
        self.port = port
        self.user = user
        self.password = password
        self.timeout = timeout
        self.token = None
        self.conn = None
        if auto_login:
            self.login()

    # ---- transport --------------------------------------------------------

    def _connect(self):
        if self.conn is None:
            self.conn = http.client.HTTPConnection(
                self.host, self.port, timeout=self.timeout)
        return self.conn

    def close(self):
        if self.conn is not None:
            try:
                self.conn.close()
            except Exception:
                pass
            self.conn = None

    def request(self, method, path, body=None, auth=True, _retry=True):
        """Raw request. Returns (status, parsed_body).

        A new connection is established automatically if the server closed an
        idle keep-alive connection, so latency measurements only pay for
        reconnection when it genuinely happens.
        """
        headers = {"Accept": "application/json"}
        payload = None
        if body is not None:
            payload = json.dumps(body)
            headers["Content-Type"] = "application/json"
        if auth and self.token:
            headers["Authorization"] = "Bearer " + self.token

        conn = self._connect()
        try:
            conn.request(method, path, body=payload, headers=headers)
            resp = conn.getresponse()
            raw = resp.read()
            status = resp.status
        except (http.client.HTTPException, socket.error, OSError):
            self.close()
            if _retry:
                return self.request(method, path, body, auth, _retry=False)
            raise

        parsed = None
        if raw:
            try:
                parsed = json.loads(raw.decode("utf-8"))
            except Exception:
                parsed = raw.decode("utf-8", "replace")
        if status < 200 or status >= 300:
            raise RestError(status, parsed, path)
        return status, parsed

    def timed(self, method, path, body=None, auth=True):
        """Same as request() but returns (elapsed_seconds, parsed_body)."""
        t0 = CLOCK()
        _, parsed = self.request(method, path, body, auth)
        return CLOCK() - t0, parsed

    # ---- API --------------------------------------------------------------

    def login(self):
        _, body = self.request(
            "POST", "/login",
            {"user": self.user, "password": self.password}, auth=False)
        if not isinstance(body, dict) or "jwt" not in body:
            raise RestError(200, body, "/login")
        self.token = body["jwt"]
        return body

    def create_graph(self, name, max_size_gb=1, description=""):
        return self.request(
            "POST", "/db",
            {"name": name, "config": {"max_size_GB": max_size_gb,
                                      "description": description}})

    def delete_graph(self, name):
        return self.request("DELETE", "/db/" + name)

    def list_graphs(self):
        _, body = self.request("GET", "/db")
        return body

    def graph_names(self):
        """Best-effort extraction of graph names from GET /db."""
        data = self.list_graphs()
        names = []
        if isinstance(data, list):
            for item in data:
                if isinstance(item, list) and item:
                    names.append(item[0])
                elif isinstance(item, dict):
                    if "name" in item:
                        names.append(item["name"])
                    elif "graph_name" in item:
                        names.append(item["graph_name"])
        elif isinstance(data, dict):
            names = list(data.keys())
        return names

    def cypher(self, script, graph="default", timeout=0):
        return self.request(
            "POST", "/cypher",
            {"script": script, "graph": graph, "timeout": timeout})

    def cypher_result(self, script, graph="default", timeout=0):
        _, body = self.cypher(script, graph, timeout)
        if isinstance(body, dict):
            return body.get("result", body)
        return body


def wait_until_ready(host, port, timeout=120.0, interval=0.2):
    """Block until the REST port accepts connections."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            s = socket.create_connection((host, port), timeout=2)
            s.close()
            return True
        except (socket.error, OSError):
            time.sleep(interval)
    return False
