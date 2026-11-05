#!/usr/bin/env bash
# Local QA entry point.
#
# Runs the pinned-container build, correctness suites, series gate, scaling
# benchmark, and the performance/soak profiles. The image ID is verified
# against images.lock on every container start.
#
# Usage:
#   qa/local/run.sh doctor
#   qa/local/run.sh build [server|image]
#   qa/local/run.sh ut
#   qa/local/run.sh it
#   qa/local/run.sh all
#   qa/local/run.sh series
#   qa/local/run.sh bench [benchmark/scaling args...]
#   qa/local/run.sh perf [smoke|full]
#   qa/local/run.sh soak [minutes]
#
# Environment:
#   QA_TEST_FILES       comma-separated pytest files for `it`
#   QA_TEST_DIAGNOSTIC  record failures without failing (baseline capture)
#   JOBS, CLEAN, BUILD_TYPE  forwarded to the server build
#   QA_DOCKER_EXTRA     extra docker flags (e.g. --cpus=4 --memory=6g)

set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/container.sh"

usage() {
    sed -n '4,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

run_container_suite() {
    local mode="$1"
    local run_id="${QA_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
    local commit
    commit="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "qa: run_id=${run_id} commit=${commit} mode=${mode}"
    QA_EXTRA_ENV="QA_TEST_MODE=${mode} QA_TEST_FILES=${QA_TEST_FILES:-} QA_TEST_DIAGNOSTIC=${QA_TEST_DIAGNOSTIC:-0} QA_RUN_ID=${run_id} QA_GIT_COMMIT=${commit}" \
        qa_run_bench bash "${QA_WORKDIR}/qa/local/entrypoint.sh"
}

CMD="${1:-help}"
shift || true

case "${CMD}" in
    doctor)
        "${HERE}/doctor.sh"
        ;;
    build)
        target="${1:-server}"
        shift || true
        case "${target}" in
            server) "${HERE}/build-server.sh" "$@" ;;
            image)  "${HERE}/build-image.sh" "$@" ;;
            *) echo "usage: qa/local/run.sh build [server|image]" >&2; exit 2 ;;
        esac
        ;;
    ut|it|all)
        run_container_suite "${CMD}"
        ;;
    series)
        "${HERE}/series_gate.sh" "$@"
        ;;
    bench)
        if [[ ! -x "${REPO_ROOT}/build/output/lgraph_server" ]]; then
            echo "qa: build/output/lgraph_server not found; run 'qa/local/run.sh build'" >&2
            exit 1
        fi
        qa_run_bench python3 "${QA_WORKDIR}/benchmark/scaling/run_bench.py" "$@"
        ;;
    perf)
        profile="${1:-smoke}"
        shift || true
        case "${profile}" in
            smoke|full) ;;
            *) echo "usage: qa/local/run.sh perf [smoke|full]" >&2; exit 2 ;;
        esac
        if [[ ! -x "${REPO_ROOT}/build/output/lgraph_server" ]]; then
            echo "qa: build/output/lgraph_server not found; run 'qa/local/run.sh build'" >&2
            exit 1
        fi
        mkdir -p "${REPO_ROOT}/qa-results"
        run_id="${QA_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
        commit="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
        QA_EXTRA_ENV="QA_TEST_MODE=perf QA_PERF_PROFILE=${profile} QA_PERF_OUT=${QA_WORKDIR}/qa-results/series-perf-${profile}.json QA_RUN_ID=${run_id} QA_GIT_COMMIT=${commit}" \
            qa_run_bench bash "${QA_WORKDIR}/qa/local/entrypoint.sh"
        ;;
    soak)
        minutes="${1:-60}"
        shift || true
        case "${minutes}" in
            ''|*[!0-9]*) echo "soak minutes must be an integer" >&2; exit 2 ;;
        esac
        if [[ ! -x "${REPO_ROOT}/build/output/lgraph_server" ]]; then
            echo "qa: build/output/lgraph_server not found; run 'qa/local/run.sh build'" >&2
            exit 1
        fi
        mkdir -p "${REPO_ROOT}/qa-results"
        run_id="${QA_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
        commit="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
        QA_EXTRA_ENV="QA_TEST_MODE=soak QA_SOAK_MINUTES=${minutes} QA_RUN_ID=${run_id} QA_GIT_COMMIT=${commit}" \
            qa_run_bench bash "${QA_WORKDIR}/qa/local/entrypoint.sh"
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        echo "unknown command: ${CMD}" >&2
        usage >&2
        exit 2
        ;;
esac
