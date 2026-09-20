#!/usr/bin/env python3
"""Multi-shard HA tester + logger.

Stands up `HA_SHARDS` independent Phase 3 HA groups (each 3 nodes) on one host
and exercises them continuously: writes a per-shard monotonic counter while
randomly killing/restarting nodes *within* shards, and logs, every
HA_LOG_EVERY seconds, each shard's leader, counter, and the per-process RSS of
every server. Validates that each shard stays independently consistent (no
cross-shard interference, no acknowledged-write loss).

This is the live counterpart to the Phase 4 control-plane unit tests: it proves
that N shards (each a replicated group) can operate simultaneously on the
available host, and measures their real memory footprint.

Usage:
    HA_SHARDS=3 HA_SOAK_MINUTES=120 HA_SOAK_INTERVAL=20 python3 ha_shard_soak.py

Environment:
    HA_WORK_DIR       scratch root (default /tmp/ha_shards)
    HA_SHARDS         number of shards, each 3 nodes (default 3)
    HA_SOAK_MINUTES   total duration in minutes (default 60)
    HA_SOAK_INTERVAL  seconds of calm between kill cycles (default 20)
    HA_LOG_EVERY      seconds between resource log lines (default 30)
"""

import json
import logging
import os
import random
import sys
import time

import ha_util
from ha_util import HAHandle, wait_for_leader, wait_for_all_healthy, \
    cypher_on_leader, SENTINEL_GRAPH

LOG = logging.getLogger("ha_shard_soak")


def vmrss_kb(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except (IOError, OSError, ValueError, IndexError):
        return 0
    return 0


def shard_rss_mib(shard):
    total = 0
    for node in shard.nodes:
        if node.proc is not None and node.proc.poll() is None:
            total += vmrss_kb(node.proc.pid)
    return total / 1024.0


def replica_value(shard, node):
    """Read the counter directly from one specific replica (no redirect)."""
    try:
        c = shard.rpc(node, retries=10)
        ok, res = c.callCypher(
            "MATCH (n:Counter {id: 0}) RETURN n.value AS v",
            SENTINEL_GRAPH, timeout=15)
        if ok:
            return json.loads(res)[0].get("v")
    except Exception:  # noqa: BLE001
        pass
    return None


def counter_read(shard, budget=30):
    deadline = time.time() + budget
    while time.time() < deadline:
        ok, res = cypher_on_leader(
            shard, "MATCH (n:Counter {id: 0}) RETURN n.value AS v",
            graph=SENTINEL_GRAPH, leader_timeout=30)
        if ok:
            try:
                return json.loads(res)[0].get("v")
            except (ValueError, TypeError, IndexError):
                pass
        time.sleep(2)
    return None


def counter_write(shard, value, budget=120):
    deadline = time.time() + budget
    while time.time() < deadline:
        ok, _ = cypher_on_leader(
            shard, "MATCH (n:Counter {id: 0}) SET n.value = %d" % value,
            graph=SENTINEL_GRAPH, leader_timeout=30)
        if ok:
            return True
        time.sleep(2)
    return False


def ensure_counter(shard):
    ok, res = cypher_on_leader(
        shard, "MATCH (n:Counter {id: 0}) RETURN count(n) AS c",
        graph=SENTINEL_GRAPH, leader_timeout=30)
    if ok and json.loads(res)[0].get("c", 0) == 0:
        cypher_on_leader(shard, "CREATE (n:Counter {id: 0, value: 0})",
                         graph=SENTINEL_GRAPH, leader_timeout=30)


def log_resources(shards, counters, last_log, log_every):
    now = time.time()
    if now - last_log < log_every:
        return last_log
    parts = []
    tot = 0.0
    for i, sh in enumerate(shards):
        mib = shard_rss_mib(sh)
        tot += mib
        lead, _ = ha_util.find_leader(sh)
        parts.append("s%d[lead=%s cnt=%s %.0fMiB]" % (i, lead, counters[i], mib))
    LOG.info("res: total=%.0fMiB %s", tot, " ".join(parts))
    return now


def main():
    duration_s = float(os.environ.get("HA_SOAK_MINUTES", "60")) * 60.0
    interval = float(os.environ.get("HA_SOAK_INTERVAL", "20"))
    log_every = float(os.environ.get("HA_LOG_EVERY", "30"))
    n_shards = int(os.environ.get("HA_SHARDS", "3"))
    base = os.environ.get("HA_WORK_DIR", "/tmp/ha_shards")

    shards = []
    for i in range(n_shards):
        h = HAHandle(os.path.join(base, "shard%d" % i))
        LOG.info("shard %d: starting 3 nodes", i)
        h.start(timeout=120.0)
        lead, _ = wait_for_leader(h, timeout=120.0)
        ha_util.setup_sentinel(h)
        ensure_counter(h)
        LOG.info("shard %d: up, leader=%s", i, lead)
        shards.append(h)

    counters = [counter_read(s) or 0 for s in shards]
    LOG.info("initial counters: %s", counters)
    kills = [0] * n_shards
    failed = [0] * n_shards
    last_log = time.time()
    end = time.time() + duration_s

    try:
        while time.time() < end:
            s = random.randrange(n_shards)
            n = random.randrange(3)
            shards[s].kill_node(n)
            kills[s] += 1
            time.sleep(2)

            # The surviving 2-node quorum of every shard must keep accepting
            # writes; this is the isolation/independence check.
            for i, sh in enumerate(shards):
                v = counters[i] + 1
                if counter_write(sh, v, budget=60):
                    counters[i] = v
                    # Append-only acknowledgement ledger: each line is one
                    # acknowledged write, so a later absolute set cannot hide it.
                    LOG.info("ledger shard=%d value=%d ts=%d",
                             i, v, int(time.time()))
                else:
                    failed[i] += 1
                    LOG.warning("shard %d: write %d failed", i, v)

            shards[s].start_node(n, timeout=120.0)
            last_log = log_resources(shards, counters, last_log, log_every)
            time.sleep(interval)

        LOG.info("duration reached; stabilising")
        for sh in shards:
            wait_for_all_healthy(sh, timeout=180.0)
        time.sleep(5)

        # Per-replica reconciliation: every replica of every shard must report
        # the last acknowledged value, not just whichever node a redirect hits.
        ok_all = True
        for i, sh in enumerate(shards):
            vals = [replica_value(sh, n) for n in range(3)]
            LOG.info("shard %d: kills=%d failed=%d acked=%d replicas=%s",
                     i, kills[i], failed[i], counters[i], vals)
            if any(v != counters[i] for v in vals):
                ok_all = False
                LOG.error("shard %d: replica divergence acked=%s replicas=%s",
                          i, counters[i], vals)
        LOG.info("shard soak %s (total kills=%d)", "OK" if ok_all else "FAILED",
                 sum(kills))
        return 0 if ok_all else 1
    finally:
        for sh in shards:
            sh.stop()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s [%(levelname)s] %(message)s")
    ha_util.LOG = logging.getLogger("ha_shard_soak")
    LOG = ha_util.LOG
    sys.exit(main())
