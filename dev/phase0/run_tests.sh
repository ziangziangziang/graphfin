#!/usr/bin/env bash
# Phase 0: run the test suites inside the pinned container.
#
# Usage:
#   dev/phase0/run_tests.sh ut                    # upstream unit tests
#   dev/phase0/run_tests.sh it                    # integration tests (pytest)
#   dev/phase0/run_tests.sh all                   # both
#   PHASE0_TEST_FILES="test_multi_graph_lifecycle.py,test_multi_graph_crud.py" \
#       dev/phase0/run_tests.sh it                # specific integration files
#                                                # (comma-separated)
#
# Policy: test failures PROPAGATE (non-zero exit) so CI cannot report success
# after a failure. Set PHASE0_TEST_DIAGNOSTIC=1 to record outcomes without
# failing (historical baseline capture); results are then kept across runs.
# Results are isolated per run and stamped with the run id and git commit.

set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/container.sh"

MODE="${1:-all}"
case "$MODE" in
    ut|it|all) ;;
    *) echo "usage: $0 [ut|it|all]" >&2; exit 2 ;;
esac

RESULTS_DIR="${REPO_ROOT}/phase0-results"
mkdir -p "${RESULTS_DIR}"

PHASE0_RUN_ID="${PHASE0_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
PHASE0_GIT_COMMIT="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "phase0: run_id=${PHASE0_RUN_ID} commit=${PHASE0_GIT_COMMIT} mode=${MODE}"

PHASE0_EXTRA_ENV="PHASE0_TEST_MODE=${MODE} PHASE0_TEST_FILES=${PHASE0_TEST_FILES:-} PHASE0_TEST_DIAGNOSTIC=${PHASE0_TEST_DIAGNOSTIC:-0} PHASE0_RUN_ID=${PHASE0_RUN_ID} PHASE0_GIT_COMMIT=${PHASE0_GIT_COMMIT}" \
    phase0_run_bench bash "${PHASE0_WORKDIR}/dev/phase0/run_tests_inner.sh"
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "phase0: test run FAILED (mode=${MODE}); see ${RESULTS_DIR}/summary.json" >&2
    exit "$rc"
fi
echo "phase0: test run complete (mode=${MODE}); see ${RESULTS_DIR}/summary.json"
