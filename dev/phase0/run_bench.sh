#!/usr/bin/env bash
# Phase 0: run the graph-count scaling benchmark inside the pinned container.
#
# Usage:
#   dev/phase0/run_bench.sh                          # 1 100 1000 4000 graphs
#   dev/phase0/run_bench.sh --quick --graphs 1 100    # smoke run
#   PHASE0_DOCKER_EXTRA='--cpus=4 --memory=6g' \
#       dev/phase0/run_bench.sh --graphs 1 1000
#
# All arguments are forwarded to benchmark/scaling/run_bench.py.
# Scratch databases live in the named Docker volume, NOT in the repo bind
# mount, so results are not distorted by the macOS virtiofs layer.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/container.sh"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    phase0_run_bench python3 "${PHASE0_WORKDIR}/benchmark/scaling/run_bench.py" --help
    exit 0
fi

if [[ ! -x "${REPO_ROOT}/build/output/lgraph_server" ]]; then
    echo "phase0: build/output/lgraph_server not found; run dev/phase0/build.sh first" >&2
    exit 1
fi

phase0_run_bench python3 "${PHASE0_WORKDIR}/benchmark/scaling/run_bench.py" "$@"
