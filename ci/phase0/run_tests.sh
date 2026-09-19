#!/usr/bin/env bash
# Phase 0: run the test suites inside the pinned container.
#
# Usage:
#   ci/phase0/run_tests.sh ut                    # upstream unit tests
#   ci/phase0/run_tests.sh it                    # integration tests (pytest)
#   ci/phase0/run_tests.sh all                   # both
#   PHASE0_TEST_FILES="test_multi_graph_lifecycle.py,test_multi_graph_crud.py" \
#       ci/phase0/run_tests.sh it                # specific integration files
#                                                # (comma-separated)
#
# Phase 0 policy: upstream failures are RECORDED, not fixed. The script exits 0
# if the harness ran at all; outcomes live in phase0-results/summary.json.

set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/container.sh"

MODE="${1:-all}"
case "$MODE" in
    ut|it|all) ;;
    *) echo "usage: $0 [ut|it|all]" >&2; exit 2 ;;
esac

RESULTS_DIR="${REPO_ROOT}/phase0-results"
mkdir -p "${RESULTS_DIR}"

PHASE0_EXTRA_ENV="PHASE0_TEST_MODE=${MODE} PHASE0_TEST_FILES=${PHASE0_TEST_FILES:-}" \
    phase0_run_bench bash "${PHASE0_WORKDIR}/ci/phase0/run_tests_inner.sh"

echo "phase0: test run complete (mode=${MODE}); see ${RESULTS_DIR}/summary.json"
