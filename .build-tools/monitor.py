#!/usr/bin/env python3
"""Sample system and compiler memory while a build runs."""
import os
import sys
import time

out = sys.argv[1]
end_file = sys.argv[2]
interval = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
PAGE = os.sysconf("SC_PAGE_SIZE")


def meminfo():
    d = {}
    with open("/proc/meminfo") as f:
        for line in f:
            k, v = line.split(":", 1)
            d[k] = int(v.strip().split()[0])
    return d


def compiler_rss():
    total = 0
    peak = 0
    names = []
    me = os.getpid()
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == me:
            continue
        try:
            with open("/proc/%s/cmdline" % pid, "rb") as f:
                cmd = f.read().replace(b"\0", b" ").decode("utf-8", "replace")
            if not any(x in cmd for x in ("cc1plus", "cc1 ", "g++ ", "collect2", "ld.gold", "ld ")):
                continue
            with open("/proc/%s/statm" % pid) as f:
                rss = int(f.read().split()[1]) * PAGE
        except OSError:
            continue
        total += rss
        if rss > peak:
            peak = rss
        base = os.path.basename(cmd.split(" ")[0])
        names.append("%s:%d" % (base, rss // 1048576))
    return total, peak, names


with open(out, "w") as f:
    f.write("ts,elapsed_s,mem_total_mb,mem_avail_mb,swap_used_mb,compiler_rss_mb,top_procs\n")
    t0 = time.time()
    while not os.path.exists(end_file):
        m = meminfo()
        total, peak, names = compiler_rss()
        f.write("%.0f,%.1f,%d,%d,%d,%d,%s\n" % (
            time.time(), time.time() - t0,
            m["MemTotal"] // 1024, m["MemAvailable"] // 1024,
            (m["SwapTotal"] - m["SwapFree"]) // 1024,
            total // 1048576, "|".join(names[:6])))
        f.flush()
        time.sleep(interval)
