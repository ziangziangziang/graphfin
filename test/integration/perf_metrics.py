"""Measurement helpers for series performance, recovery and soak tests.

Standard library only: the test tree is staged into build/output where no
third-party packages are guaranteed beyond pytest and the client bindings.
"""

import hashlib
import json
import os
import platform
import stat
import threading
import time

DEFAULT_PROFILE = "smoke"


def profile():
    return os.environ.get("QA_PERF_PROFILE", DEFAULT_PROFILE)


def is_full():
    return profile() == "full"


def pick(smoke_value, full_value):
    return full_value if is_full() else smoke_value


def percentile(values, q):
    if not values:
        return None
    xs = sorted(values)
    return xs[min(len(xs) - 1, int(len(xs) * q))]


def latency_summary(seconds):
    """Millisecond summary from end-to-end wall-clock samples."""
    if not seconds:
        return {"count": 0}
    ms = [s * 1000.0 for s in seconds]
    return {
        "count": len(ms),
        "p50_ms": round(percentile(ms, 0.50), 3),
        "p95_ms": round(percentile(ms, 0.95), 3),
        "p99_ms": round(percentile(ms, 0.99), 3),
        "max_ms": round(max(ms), 3),
    }


def dir_usage(path):
    """Apparent and allocated bytes plus file count for a directory tree.

    LMDB reserves a large sparse map, so apparent size alone overstates the
    physical footprint; st_blocks reflects what the filesystem actually
    allocated.
    """
    apparent = allocated = 0
    files = 0
    for root, _dirs, names in os.walk(path):
        for name in names:
            p = os.path.join(root, name)
            try:
                st = os.lstat(p)
            except OSError:
                continue
            if not stat.S_ISREG(st.st_mode):
                continue
            files += 1
            apparent += st.st_size
            allocated += getattr(st, "st_blocks", 0) * 512
    return {
        "apparent_bytes": apparent,
        "allocated_bytes": allocated,
        "files": files,
    }


def read_status(pid):
    out = {}
    try:
        with open("/proc/%d/status" % pid, "r") as f:
            for line in f:
                if ":" in line:
                    key, value = line.split(":", 1)
                    out[key.strip()] = value.strip()
    except (IOError, OSError):
        pass
    return out


def _kb(status, key):
    try:
        return int(status.get(key, "").split()[0])
    except (ValueError, IndexError):
        return 0


def rss_peak_kb(pid):
    return _kb(read_status(pid), "VmHWM")


def process_snapshot(pid, with_fds=False):
    """Current RSS/VM/thread footprint; empty dict if the process is gone."""
    st = read_status(pid)
    if not st:
        return {}
    return {
        "rss_kb": _kb(st, "VmRSS"),
        "vmsize_kb": _kb(st, "VmSize"),
        "vmpeak_kb": _kb(st, "VmPeak"),
        "threads": _kb(st, "Threads"),
        "fds": count_fds(pid) if with_fds else None,
    }


def process_io(pid):
    out = {}
    try:
        with open("/proc/%d/io" % pid, "r") as f:
            for line in f:
                key, value = line.split(":", 1)
                out[key.strip()] = int(value.strip())
    except (IOError, OSError, ValueError):
        pass
    return out


def count_fds(pid):
    try:
        return len(os.listdir("/proc/%d/fd" % pid))
    except (IOError, OSError):
        return -1


class RssSampler(object):
    """Background sampler for the server's memory and descriptor footprint."""

    def __init__(self, pid, interval=0.5, with_fds=False):
        self.pid = pid
        self.interval = interval
        self.with_fds = with_fds
        self._stop = threading.Event()
        self._thread = None
        self._samples = []
        self._peaks = {
            "rss_kb": 0,
            "vmsize_kb": 0,
            "vmpeak_kb": 0,
            "threads": 0,
            "fds": 0,
        }

    def _sample(self):
        st = read_status(self.pid)
        if not st:
            return None
        s = {
            "t": time.monotonic(),
            "rss_kb": _kb(st, "VmRSS"),
            "vmsize_kb": _kb(st, "VmSize"),
            "vmpeak_kb": _kb(st, "VmPeak"),
            "threads": _kb(st, "Threads"),
            "fds": count_fds(self.pid) if self.with_fds else 0,
        }
        for key, value in s.items():
            if key != "t" and value > self._peaks[key]:
                self._peaks[key] = value
        return s

    def _run(self):
        while not self._stop.is_set():
            s = self._sample()
            if s is not None:
                self._samples.append(s)
            self._stop.wait(self.interval)

    def start(self):
        s = self._sample()
        if s is not None:
            self._samples.append(s)
        self._thread = threading.Thread(target=self._run)
        self._thread.daemon = True
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)
        s = self._sample()
        if s is not None:
            self._samples.append(s)
        return self.summary()

    def summary(self):
        first_t = self._samples[0]["t"] if self._samples else None
        series = []
        for s in self._samples[:2000]:
            entry = dict(s)
            if first_t is not None:
                entry["t_rel"] = round(s["t"] - first_t, 3)
            entry.pop("t", None)
            series.append(entry)
        return {
            "samples": len(self._samples),
            "peaks": dict(self._peaks),
            "first": series[0] if series else None,
            "last": series[-1] if series else None,
            "time_series": series,
        }


def fingerprint(points):
    """SHA-256 over the ordered, canonical point payload."""
    digest = hashlib.sha256()
    for point in points:
        digest.update(json.dumps(point, sort_keys=True,
                                  separators=(",", ":")).encode("utf-8"))
        digest.update(b"\n")
    return digest.hexdigest()


def env_probe():
    cpu_model = ""
    try:
        with open("/proc/cpuinfo", "r") as f:
            for line in f:
                if line.startswith("model name"):
                    cpu_model = line.split(":", 1)[1].strip()
                    break
    except (IOError, OSError):
        pass
    memory_kb = 0
    try:
        with open("/proc/meminfo", "r") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    memory_kb = int(line.split()[1])
                    break
    except (IOError, OSError, ValueError, IndexError):
        pass

    binary = os.environ.get("QA_SERVER_BINARY", "lgraph_server")
    binary_info = {}
    if os.path.isfile(binary):
        st = os.stat(binary)
        binary_info = {"path": os.path.abspath(binary),
                       "size_bytes": st.st_size,
                       "mtime": int(st.st_mtime)}

    return {
        "arch": platform.machine(),
        "kernel": platform.release(),
        "python": platform.python_version(),
        "cpu_count": os.cpu_count(),
        "cpu_model": cpu_model,
        "memory_total_kb": memory_kb,
        "git_commit": os.environ.get("QA_GIT_COMMIT", ""),
        "profile": profile(),
        "binary": binary_info,
    }


def record(section, data):
    """Merge one result section into $QA_PERF_OUT and write it atomically."""
    path = os.environ.get("QA_PERF_OUT")
    if not path:
        return None
    directory = os.path.dirname(os.path.abspath(path))
    if directory and not os.path.isdir(directory):
        os.makedirs(directory)
    doc = {}
    if os.path.isfile(path):
        try:
            with open(path, "r") as f:
                doc = json.load(f)
        except (ValueError, IOError, OSError):
            doc = {}
    doc.setdefault("schema", 1)
    doc["profile"] = profile()
    doc.setdefault("env", env_probe())
    doc.setdefault("sections", {})[section] = data
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, path)
    return path
