"""Sample a running process's resource usage from /proc.

Measures exactly the quantities Phase 0 must record for each graph count:
RSS, virtual size, thread count, file descriptor count and memory-mapping
count. Everything comes from /proc, so there are no dependencies.

Standard library only (Python 3.6 compatible).
"""

import os
import stat as stat_mod
import threading
import time

CLOCK = time.monotonic


def read_status(pid):
    """Parse /proc/<pid>/status into a dict of strings."""
    out = {}
    try:
        with open("/proc/%d/status" % pid, "r") as f:
            for line in f:
                if ":" in line:
                    k, v = line.split(":", 1)
                    out[k.strip()] = v.strip()
    except (IOError, OSError):
        pass
    return out


def _kb(status, key):
    v = status.get(key, "")
    if not v:
        return 0
    try:
        return int(v.split()[0])
    except (ValueError, IndexError):
        return 0


def count_fds(pid):
    try:
        return len(os.listdir("/proc/%d/fd" % pid))
    except (IOError, OSError):
        return -1


def count_maps(pid):
    """Count memory mappings. O(maps) to read, so call it sparingly."""
    n = 0
    try:
        with open("/proc/%d/maps" % pid, "r") as f:
            for _ in f:
                n += 1
    except (IOError, OSError):
        return -1
    return n


def sample(pid, with_maps=False):
    """One measurement of the process. Returns a dict, or None if it is gone."""
    st = read_status(pid)
    if not st:
        return None
    s = {
        "t": CLOCK(),
        "rss_kb": _kb(st, "VmRSS"),
        "vmsize_kb": _kb(st, "VmSize"),
        "vmpeak_kb": _kb(st, "VmPeak"),
        "threads": _kb(st, "Threads"),
        "fds": count_fds(pid),
    }
    if with_maps:
        s["maps"] = count_maps(pid)
    return s


class Sampler(object):
    """Background sampler tracking peak values of a process.

    Cheap metrics are sampled every `interval` seconds. `maps` is expensive at
    high graph counts (it is O(number of mappings)), so it is sampled only
    every `maps_every` ticks.
    """

    def __init__(self, pid, interval=0.5, maps_every=4, max_points=600):
        self.pid = pid
        self.interval = interval
        self.maps_every = maps_every
        self.max_points = max_points
        self._stop = threading.Event()
        self._thread = None
        self._samples = []
        self._peaks = {"rss_kb": 0, "vmsize_kb": 0, "threads": 0, "fds": 0,
                       "maps": 0, "vmpeak_kb": 0}
        self._first = None
        self._last = None

    def _record(self, s):
        if s is None:
            return
        if self._first is None:
            self._first = s
        self._last = s
        for k in self._peaks:
            if k in s and s[k] > self._peaks[k]:
                self._peaks[k] = s[k]
        self._samples.append(s)

    def _run(self):
        tick = 0
        while not self._stop.is_set():
            with_maps = (tick % self.maps_every == 0)
            self._record(sample(self.pid, with_maps=with_maps))
            tick += 1
            self._stop.wait(self.interval)

    def start(self):
        # Give the child a moment to exec before the first sample. Sampling
        # immediately after Popen can catch the pre-exec forked process
        # (1 thread, a few hundred KiB RSS, ~10 mappings), which would show up
        # as the "startup peak" and be wrong by orders of magnitude.
        self._stop.wait(0.05)
        self._record(sample(self.pid, with_maps=True))
        self._thread = threading.Thread(target=self._run)
        self._thread.daemon = True
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=5)
        self._record(sample(self.pid, with_maps=True))
        return self.summary()

    def _downsample(self):
        n = len(self._samples)
        if n <= self.max_points:
            return self._samples
        step = max(1, n // self.max_points)
        out = self._samples[::step]
        if out[-1] is not self._samples[-1]:
            out.append(self._samples[-1])
        return out

    def summary(self):
        first_t = self._first["t"] if self._first else None
        series = []
        for s in self._downsample():
            e = dict(s)
            if first_t is not None:
                e["t_rel"] = round(s["t"] - first_t, 3)
            e.pop("t", None)
            series.append(e)
        return {
            "samples": len(self._samples),
            "peaks": dict(self._peaks),
            "first": self._first,
            "last": self._last,
            "time_series": series,
        }


class PhaseTimer(object):
    """Context manager measuring wall-clock duration of a named phase."""

    def __init__(self, results, name):
        self.results = results
        self.name = name

    def __enter__(self):
        self.t0 = CLOCK()
        return self

    def __exit__(self, exc_type, exc, tb):
        self.results[self.name] = {
            "seconds": round(CLOCK() - self.t0, 4),
            "error": None if exc is None else "%s: %s" % (exc_type.__name__, exc),
        }
        return False


def dir_size_bytes(path):
    """Recursive on-disk size of a directory tree (apparent size, bytes)."""
    total = 0
    for root, dirs, files in os.walk(path):
        for name in files:
            p = os.path.join(root, name)
            try:
                st = os.lstat(p)
                if stat_mod.S_ISREG(st.st_mode):
                    total += st.st_size
            except (IOError, OSError):
                pass
    return total


def count_files(path):
    n = 0
    for root, dirs, files in os.walk(path):
        n += len(files)
    return n
