"""Capture machine/environment metadata for reproducible Phase 0 results.

Every benchmark run records this so a number is always interpretable: a
startup time without the CPU, RAM, fd limit and cgroup limits next to it is
not a reproducible measurement.

Standard library only (the pinned image ships Python 3.6.9 and has no network
access for pip installs).
"""

import json
import os
import platform
import resource
import subprocess
import sys
import time

PHASE0_LIB_VERSION = "1.0.0"


def _read(path, default=""):
    try:
        with open(path, "r") as f:
            return f.read().strip()
    except Exception:
        return default


def _run(cmd, cwd=None):
    try:
        p = subprocess.Popen(
            cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            universal_newlines=True)
        out, _ = p.communicate(timeout=10)
        return out.strip() if p.returncode == 0 else ""
    except Exception:
        return ""


def _cgroup_memory_limit():
    v = _read("/sys/fs/cgroup/memory.max")            # cgroup v2
    if v and v != "max":
        return v
    v = _read("/sys/fs/cgroup/memory/memory.limit_in_bytes")   # cgroup v1
    if v:
        return v
    return "unknown"


def _cgroup_pids_limit():
    for p in ("/sys/fs/cgroup/pids.max",
              "/sys/fs/cgroup/pids/pids.max"):
        v = _read(p)
        if v:
            return v
    return "unknown"


def _cgroup_cpu_limit():
    v = _read("/sys/fs/cgroup/cpu.max")               # cgroup v2: "quota period"
    if v and v != "max":
        return v
    quota = _read("/sys/fs/cgroup/cpu/cpu.cfs_quota_us")
    period = _read("/sys/fs/cgroup/cpu/cpu.cfs_period_us")
    if quota and period:
        return "%s %s" % (quota, period)
    return "unknown"


def _cpu_model():
    """Best-effort CPU identification across x86 and arm64 /proc/cpuinfo."""
    fields = {}
    for line in _read("/proc/cpuinfo").splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            fields[k.strip().lower()] = v.strip()
    for key in ("model name", "hardware", "cpu model"):
        if key in fields:
            return fields[key]
    # arm64 exposes implementer/part instead of a model name.
    if "cpu part" in fields or "cpu implementer" in fields:
        parts = []
        for key in ("cpu implementer", "cpu part", "cpu architecture", "cpu variant"):
            if key in fields:
                parts.append("%s=%s" % (key.replace("cpu ", ""), fields[key]))
        return "ARM (%s)" % ", ".join(parts)
    return platform.processor() or "unknown"


def _meminfo():
    out = {}
    for line in _read("/proc/meminfo").splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            out[k.strip()] = v.strip()
    return out


def _fs_type(path):
    """Return (fstype, mountpoint) for the filesystem containing path."""
    if not path:
        return "unknown", ""
    path = os.path.abspath(path)
    best_mp, best_fs = "", "unknown"
    for line in _read("/proc/mounts").splitlines():
        f = line.split()
        if len(f) < 3:
            continue
        mp = f[1].replace("\\040", " ")
        if path == mp or path.startswith(mp.rstrip("/") + "/"):
            if len(mp) >= len(best_mp):
                best_mp, best_fs = mp, f[2]
    return best_fs, best_mp


def _os_release():
    d = {}
    for line in _read("/etc/os-release").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            d[k] = v.strip().strip('"')
    if not d:
        d["PRETTY_NAME"] = _read("/etc/redhat-release") or platform.platform()
    return d


def _affinity_count():
    try:
        return len(os.sched_getaffinity(0))
    except Exception:
        return os.cpu_count()


def capture(data_dir=None, repo_root=None):
    """Capture a full environment description as a JSON-serialisable dict."""
    rl = resource.getrlimit(resource.RLIMIT_NOFILE)
    mi = _meminfo()
    fs_type, fs_mp = _fs_type(data_dir)
    osr = _os_release()

    env = {
        "lib_version": PHASE0_LIB_VERSION,
        "captured_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": {
            "hostname": platform.node(),
            "arch": platform.machine(),
            "kernel": platform.release(),
            "platform": platform.platform(),
            "os_release": osr.get("PRETTY_NAME", "unknown"),
            "os_id": osr.get("ID", "unknown"),
        },
        "cpu": {
            "model": _cpu_model(),
            "count": os.cpu_count(),
            "affinity": _affinity_count(),
        },
        "memory": {
            "mem_total_kb": mi.get("MemTotal", "unknown"),
            "mem_available_kb": mi.get("MemAvailable", "unknown"),
            "swap_total_kb": mi.get("SwapTotal", "unknown"),
            "cgroup_limit_bytes": _cgroup_memory_limit(),
        },
        "limits": {
            "nofile_soft": rl[0],
            "nofile_hard": rl[1],
            "max_map_count": _read("/proc/sys/vm/max_map_count", "unknown"),
            "cgroup_pids_max": _cgroup_pids_limit(),
            "cgroup_cpu_max": _cgroup_cpu_limit(),
            "threads_max": _read("/proc/sys/kernel/threads-max", "unknown"),
        },
        "storage": {
            "data_dir": data_dir or "",
            "data_dir_fstype": fs_type,
            "data_dir_mountpoint": fs_mp,
        },
        "python": {
            "version": sys.version.split()[0],
            "executable": sys.executable,
        },
    }

    # Image identity, when running in the pinned container. This is the single
    # most important reproducibility anchor, so try hard to record it.
    env["container"] = {
        "image_id": os.environ.get("PHASE0_IMAGE_ID", ""),
        "cgroup": _read("/proc/self/cgroup", ""),
    }

    if repo_root:
        env["source"] = {
            "git_commit": _run(["git", "rev-parse", "HEAD"], cwd=repo_root),
            "git_describe": _run(["git", "describe", "--always", "--dirty"], cwd=repo_root),
        }
    return env


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Capture Phase 0 environment metadata")
    ap.add_argument("--data-dir", default=os.environ.get("PHASE0_DATA_DIR", ""))
    ap.add_argument("--repo-root", default=os.environ.get("PHASE0_REPO_ROOT", "."))
    args = ap.parse_args()
    print(json.dumps(capture(args.data_dir, args.repo_root), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
