#!/usr/bin/env python3
"""Long-running HA soak / chaos driver.

Keeps ONE 3-node lgraph_server cluster alive and, for a configurable duration,
continuously writes a monotonically increasing counter while randomly killing
and restarting nodes. It validates that every acknowledged write survives
(and that the counter never regresses) — the PROJECT.md Phase 3 chaos loop.

Usage:
    HA_SOAK_MINUTES=120 HA_SOAK_INTERVAL=20 python3 ha_soak.py

Environment:
    HA_WORK_DIR       scratch dir (default /tmp/ha_soak)
    HA_SOAK_MINUTES   total duration in minutes (default 60)
    HA_SOAK_INTERVAL  seconds of calm between kill cycles (default 20)

Exit code 0 if no acknowledged write was lost and the cluster stayed usable at
the end; non-zero otherwise. Progress is logged to stdout.
"""

import json
import logging
import os
import random
import sys
import time

import ha_util
from ha_util import HAHandle, SENTINEL_GRAPH

LOG = ha_util.LOG


def op(h, script, graph, budget=120):
    """Run a statement via a live node (writes redirect to the leader)."""
    deadline = time.time() + budget
    last = None
    while time.time() < deadline:
        try:
            c = ha_util.live_client(h)
            ok, res = c.callCypher(script, graph, timeout=20)
            if ok:
                return True, res
            last = res
        except Exception as exc:  # noqa: BLE001
            last = exc
        time.sleep(2)
    return False, last


def ensure_up(h):
    for i in range(3):
        if not h.nodes[i].alive():
            LOG.info("soak: restarting dead node %d", i)
            h.start_node(i, timeout=120.0)


def main():
    duration_s = float(os.environ.get("HA_SOAK_MINUTES", "60")) * 60.0
    interval = float(os.environ.get("HA_SOAK_INTERVAL", "20"))
    work = os.environ.get("HA_WORK_DIR", "/tmp/ha_soak")

    h = HAHandle(work)
    h.start(timeout=120.0)
    ha_util.wait_for_leader(h, timeout=90.0)
    LOG.info("soak: cluster up; duration=%.0fs interval=%.0fs", duration_s, interval)

    op(h, "CALL dbms.graph.createGraph('%s', 'soak', 1)" % SENTINEL_GRAPH, "default")
    op(h, "CALL db.createVertexLabel('Counter', 'id', 'id', 'INT64', false, "
          "'value', 'INT64', false)", SENTINEL_GRAPH)
    op(h, "CREATE (n:Counter {id: 0, value: 0})", SENTINEL_GRAPH)

    last = 0
    acked = 0
    failed_writes = 0
    kills = 0
    end = time.time() + duration_s

    try:
        while time.time() < end:
            ensure_up(h)
            victim = random.randrange(3)
            h.kill_node(victim)
            kills += 1
            time.sleep(2)

            v = last + 1
            ok, res = op(h, "MATCH (n:Counter {id: 0}) SET n.value = %d" % v,
                         SENTINEL_GRAPH, budget=120)
            if ok:
                last = v
                acked += 1
            else:
                failed_writes += 1
                LOG.warning("soak: write %d failed: %s", v, res)

            h.start_node(victim, timeout=120.0)
            if kills % 5 == 0:
                LOG.info("soak: progress kills=%d acked=%d failed=%d remaining=%.0fs",
                         kills, acked, failed_writes, max(0, end - time.time()))
            time.sleep(interval)

        ensure_up(h)
        ha_util.wait_for_all_healthy(h, timeout=180.0)
        time.sleep(5)

        got = None
        deadline = time.time() + 120
        while time.time() < deadline:
            ok, res = op(h, "MATCH (n:Counter {id: 0}) RETURN n.value AS v",
                         SENTINEL_GRAPH, budget=30)
            if ok:
                got = json.loads(res)[0].get("v")
                if got == last:
                    break
            time.sleep(3)

        LOG.info("soak: DONE kills=%d acked=%d failed=%d final_acked=%d read=%s",
                 kills, acked, failed_writes, last, got)
        if got != last:
            LOG.error("soak: DATA LOSS/REGRESSION: acked=%s read=%s", last, got)
            return 1
        return 0
    finally:
        h.stop()


if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(message)s")
    ha_util.LOG = logging.getLogger("ha_soak")
    LOG = ha_util.LOG
    sys.exit(main())
