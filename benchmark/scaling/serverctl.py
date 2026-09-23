"""Launch, measure and stop an lgraph_server process.

Startup is defined as the interval from `Popen` to the moment the server logs
"Server started." (src/server/lgraph_server.cpp:379). That line is emitted only
after Galaxy has opened every graph, so it is exactly the metric that is
sensitive to graph count. Shutdown is SIGTERM to process exit.

Standard library only (Python 3.6 compatible).
"""

import json
import os
import shutil
import signal
import socket
import subprocess
import time

import procmetrics

CLOCK = time.monotonic
READY_MARKER = "Server started."


def find_free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class LogTailer(object):
    """Incrementally read a growing log file."""

    def __init__(self, path):
        self.path = path
        self._pos = 0
        self._buf = ""

    def poll(self):
        """Return newly appended text since the last call."""
        try:
            with open(self.path, "r") as f:
                f.seek(self._pos)
                chunk = f.read()
                self._pos = f.tell()
        except (IOError, OSError):
            return ""
        if chunk:
            self._buf += chunk
            # keep memory bounded on very chatty runs
            if len(self._buf) > 8 * 1024 * 1024:
                self._buf = self._buf[-4 * 1024 * 1024:]
        return chunk

    def text(self):
        return self._buf

    def tail(self, n=60):
        lines = self._buf.splitlines()
        return "\n".join(lines[-n:])


class ServerError(RuntimeError):
    pass


class Server(object):
    def __init__(self, binary, db_dir, http_port=None, rpc_port=None,
                 bolt_port=None, config_path=None, extra_args=None,
                 log_path=None, enable_rpc=True, startup_timeout=1800.0,
                 poll_interval=0.02, sampler_interval=0.5):
        self.binary = binary
        self.db_dir = os.path.abspath(db_dir)
        self.http_port = http_port or find_free_port()
        self.rpc_port = rpc_port or find_free_port()
        self.bolt_port = bolt_port or find_free_port()
        self.config_path = config_path
        self.extra_args = list(extra_args or [])
        self.log_path = log_path or os.path.join(
            os.path.dirname(self.db_dir), "server.log")
        self.enable_rpc = enable_rpc
        self.startup_timeout = startup_timeout
        self.poll_interval = poll_interval
        self.sampler_interval = sampler_interval

        self.proc = None
        self._logf = None
        self.tailer = None
        self.sampler = None
        self.startup_seconds = None
        self.shutdown_seconds = None
        self._started = False

    # ---- command line -----------------------------------------------------

    def cmd(self):
        c = [self.binary]
        if self.config_path:
            c += ["-c", self.config_path]
        c += [
            "--directory", self.db_dir,
            "--host", "127.0.0.1",
            "--port", str(self.http_port),
            "--enable_rpc", "true" if self.enable_rpc else "false",
            "--rpc_port", str(self.rpc_port),
            "--bolt_port", str(self.bolt_port),
            "--verbose", "1",
        ]
        c += self.extra_args
        return c

    # ---- lifecycle --------------------------------------------------------

    def start(self):
        if self.proc is not None:
            raise ServerError("server already started")
        os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
        # Truncate the log so the readiness marker cannot be a stale match.
        self._logf = open(self.log_path, "w")
        self.tailer = LogTailer(self.log_path)

        t0 = CLOCK()
        self.proc = subprocess.Popen(
            self.cmd(), stdout=self._logf, stderr=subprocess.STDOUT,
            close_fds=True, cwd=os.path.dirname(self.db_dir))
        self.sampler = procmetrics.Sampler(
            self.proc.pid, interval=self.sampler_interval).start()

        deadline = t0 + self.startup_timeout
        while True:
            new = self.tailer.poll()
            if new and READY_MARKER in new:
                # Also make sure the marker is in the accumulated buffer, in
                # case it straddled two reads.
                if READY_MARKER in self.tailer.text():
                    self.startup_seconds = CLOCK() - t0
                    self._started = True
                    return self.startup_seconds
            rc = self.proc.poll()
            if rc is not None:
                self._finish_sampler()
                raise ServerError(
                    "server exited with code %s before logging %r\n--- log tail ---\n%s"
                    % (rc, READY_MARKER, self.tailer.tail(80)))
            if CLOCK() > deadline:
                self.kill()
                raise ServerError(
                    "server did not log %r within %.0fs\n--- log tail ---\n%s"
                    % (READY_MARKER, self.startup_timeout, self.tailer.tail(80)))
            time.sleep(self.poll_interval)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def _finish_sampler(self):
        if self.sampler is not None:
            self.sampler.stop()
            self.sampler = None

    def stop(self, timeout=300.0):
        """SIGTERM and wait for exit. Returns shutdown seconds."""
        if self.proc is None or self.proc.poll() is not None:
            raise ServerError("server is not running")
        t0 = CLOCK()
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.kill()
            raise ServerError(
                "server did not exit within %.0fs of SIGTERM (SIGKILLed)\n"
                "--- log tail ---\n%s" % (timeout, self.tailer.tail(80)))
        self.shutdown_seconds = CLOCK() - t0
        self._finish_sampler()
        self._close_log()
        return self.shutdown_seconds

    def kill(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                pass
        self._finish_sampler()
        self._close_log()

    def _close_log(self):
        if self._logf is not None:
            try:
                self._logf.close()
            except Exception:
                pass
            self._logf = None

    def wait_exit(self, timeout=300.0):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return None

    # ---- introspection ---------------------------------------------------

    def samples(self):
        """Sampler summary. Must be called before stop()/kill() finalises it."""
        if self.sampler is None:
            return None
        return self.sampler.summary()

    def log_text(self):
        return self.tailer.text() if self.tailer else ""

    def log_tail(self, n=80):
        return self.tailer.tail(n) if self.tailer else ""

    def url(self, path=""):
        return "http://127.0.0.1:%d/%s" % (self.http_port, path.lstrip("/"))


def fresh_dir(path):
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)
    return path


def make_min_config(path):
    """Write a minimal server config JSON.

    All graph-count-relevant settings are still passed on the command line so
    that the config file cannot silently change between runs.
    """
    cfg = {
        "host": "127.0.0.1",
        "verbose": 1,
        "enable_rpc": True,
        "ssl_auth": False,
        "disable_auth": False,
        "enable_backup_log": False,
    }
    with open(path, "w") as f:
        json.dump(cfg, f, indent=2)
    return path
