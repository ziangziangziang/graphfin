#!/usr/bin/env bash
# Test execution, run INSIDE the compile container.
#
# Invoked by qa/local/run.sh. Reads:
#   QA_TEST_MODE       ut | it | all | perf | soak
#   QA_TEST_FILES      optional comma-separated pytest file list (it mode)
#   QA_PERF_PROFILE    smoke | full (perf mode)
#   QA_PERF_OUT        result JSON path (perf mode)
#   QA_SOAK_MINUTES    soak duration (soak mode)
#
# Failures PROPAGATE by default: the script exits non-zero if any suite fails.
# Set QA_TEST_DIAGNOSTIC=1 to record ut/it outcomes without failing (useful
# when capturing a known-flaky baseline). Perf and soak failures are never
# diagnostic-only.

set -uo pipefail

REPO="${QA_WORKDIR:-/workspace}"
OUT="${REPO}/qa-results"
MODE="${QA_TEST_MODE:-all}"
DIAGNOSTIC="${QA_TEST_DIAGNOSTIC:-0}"
mkdir -p "$OUT"

# Isolate results by run: wipe the machine-readable outputs unless explicitly
# asked to keep history. Without this a stale unit_test.xml/ut.log from a
# previous run leaks into an unrelated mode's summary (observed: an `it` run
# reporting a unit_test exit code).
if [ "$DIAGNOSTIC" != "1" ]; then
    rm -f "$OUT"/ut.log "$OUT"/it.log "$OUT"/perf.log "$OUT"/soak.log \
          "$OUT"/unit_test.xml "$OUT"/summary.json
fi

FAILURES=0
mark_fail() { FAILURES=1; }

cd "${REPO}/build/output" || exit 3
if [ ! -e data ] && [ -d ../../test/integration/data ]; then
    ln -sfn ../../test/integration/data data
fi
export LD_LIBRARY_PATH="/usr/local/lib64/lgraph:/usr/local/lib64:/usr/local/lib:${REPO}/build/output:${LD_LIBRARY_PATH:-}"
export OMP_NUM_THREADS=2

stage_tests() {
    cp -r "${REPO}"/test/integration/* . 2>/dev/null
    cp -r "${REPO}"/src/client/python/TuGraphClient/*.py . 2>/dev/null
}

run_ut() {
    local fail=0
    echo "=== fma_unit_test (upstream) ===" | tee "$OUT/ut.log"
    ./fma_unit_test -t all >>"$OUT/ut.log" 2>&1
    local rc=$?
    echo "fma_unit_test exit=$rc" | tee -a "$OUT/ut.log"
    [ "$rc" -ne 0 ] && fail=1

    echo "=== unit_test (upstream, gtest) ===" | tee -a "$OUT/ut.log"
    rm -rf testdb* .import_tmp 2>/dev/null
    ./unit_test --gtest_output=xml:"$OUT/unit_test.xml" >>"$OUT/ut.log" 2>&1
    rc=$?
    echo "unit_test exit=$rc" | tee -a "$OUT/ut.log"
    rm -rf testdb* .import_tmp 2>/dev/null
    [ "$rc" -ne 0 ] && fail=1
    return "$fail"
}

run_it() {
    echo "=== integration tests (pytest) ===" | tee "$OUT/it.log"
    cd "${REPO}/build/output" || return 3
    stage_tests

    mkdir -p learn/examples 2>/dev/null
    cp -r "${REPO}"/learn/examples/* learn/examples/ 2>/dev/null
    # ci/github_ci.sh copies demo/movie into cwd; test_http_server.py reads
    # ./movie/import.json.
    cp -r "${REPO}"/demo/movie . 2>/dev/null
    # cpp client test binary (ci/github_ci.sh:68-71), built out-of-source so
    # the source tree stays clean.
    mkdir -p "${REPO}"/build/clienttest
    cmake -S "${REPO}"/test/test_rpc_client/cpp/CppClientTest \
          -B "${REPO}"/build/clienttest >>"$OUT/it.log" 2>&1
    cmake --build "${REPO}"/build/clienttest -j2 >>"$OUT/it.log" 2>&1
    cp "${REPO}"/build/clienttest/clienttest . 2>/dev/null
    # BUILD_PROCEDURE=OFF: the algorithm/procedure suites cannot run.
    # Exclusion list from test/suites.json; suites.py fails loudly on errors.
    # shellcheck disable=SC2046
    rm -f $(python3 "${REPO}/test/suites.py" --suite main-it --field procedure_off_exclusions)

    local marker
    marker="$(python3 "${REPO}/test/suites.py" --suite main-it --field pytest_marker)" || {
        echo "qa: could not read main-it marker from test/suites.json" >&2
        return 3
    }
    if [ -n "${QA_TEST_FILES:-}" ]; then
        # QA_TEST_FILES is comma-separated because the value travels through
        # `docker run -e` via a whitespace-split variable list.
        FILES="$(echo "${QA_TEST_FILES}" | tr ',' ' ')"
        # shellcheck disable=SC2086
        if [ -n "$marker" ]; then
            python3 -m pytest ${FILES} -m "$marker" -v -p no:cacheprovider >>"$OUT/it.log" 2>&1
        else
            python3 -m pytest ${FILES} -v -p no:cacheprovider >>"$OUT/it.log" 2>&1
        fi
    else
        if [ -n "$marker" ]; then
            python3 -m pytest . -m "$marker" -v -p no:cacheprovider >>"$OUT/it.log" 2>&1
        else
            python3 -m pytest . -v -p no:cacheprovider >>"$OUT/it.log" 2>&1
        fi
    fi
    local rc=$?
    echo "pytest exit=$rc" | tee -a "$OUT/it.log"
    return "$rc"
}

perf_files() {
    # The perf modules live beside the integration tests so the existing
    # build/output staging works unchanged.
    local files
    files="$(ls test_perf_*.py 2>/dev/null || true)"
    if [ -z "$files" ]; then
        echo "qa: no test_perf_*.py files staged in ${REPO}/build/output" >&2
        return 1
    fi
    printf '%s\n' "$files"
}

run_perf() {
    local profile="${QA_PERF_PROFILE:-smoke}"
    local marker="perf_smoke"
    [ "$profile" = "full" ] && marker="perf"
    export QA_PERF_PROFILE="$profile"
    export QA_PERF_OUT="${QA_PERF_OUT:-${OUT}/series-perf-${profile}.json}"
    # Never merge this run into a previous run's sections: a stale workload
    # scale or memory peak must not survive a failed or narrower run.
    rm -f "$QA_PERF_OUT"

    echo "=== series performance (${profile}) ===" | tee "$OUT/perf.log"
    cd "${REPO}/build/output" || return 3
    stage_tests

    local files
    files="$(perf_files)" || return 1
    # shellcheck disable=SC2086
    python3 -m pytest ${files} -m "$marker" -v -p no:cacheprovider >>"$OUT/perf.log" 2>&1
    local rc=$?
    echo "pytest exit=$rc" | tee -a "$OUT/perf.log"
    return "$rc"
}

run_soak() {
    local minutes="${QA_SOAK_MINUTES:-60}"
    export QA_SOAK_MINUTES="$minutes"
    export QA_PERF_PROFILE=soak
    export QA_PERF_OUT="${QA_PERF_OUT:-${OUT}/series-perf-soak.json}"
    rm -f "$QA_PERF_OUT"

    echo "=== series soak (${minutes} min) ===" | tee "$OUT/soak.log"
    cd "${REPO}/build/output" || return 3
    stage_tests

    local files
    files="$(perf_files)" || return 1
    # shellcheck disable=SC2086
    python3 -m pytest ${files} -m soak -v -p no:cacheprovider >>"$OUT/soak.log" 2>&1
    local rc=$?
    echo "pytest exit=$rc" | tee -a "$OUT/soak.log"
    return "$rc"
}

case "$MODE" in
    ut)   run_ut || mark_fail ;;
    it)   run_it || mark_fail ;;
    all)  run_ut || mark_fail; run_it || mark_fail ;;
    perf) run_perf || mark_fail ;;
    soak) run_soak || mark_fail ;;
    *)    echo "unknown QA_TEST_MODE=$MODE" >&2; exit 2 ;;
esac

# --- machine-readable summary ---------------------------------------------
python3 - "$OUT" <<'PY'
import json, os, re, sys, time
out = sys.argv[1]
summary = {
    "mode": os.environ.get("QA_TEST_MODE", "?"),
    "run_id": os.environ.get("QA_RUN_ID", ""),
    "git_commit": os.environ.get("QA_GIT_COMMIT", ""),
    "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "diagnostic": os.environ.get("QA_TEST_DIAGNOSTIC", "0") == "1",
}
if os.environ.get("QA_PERF_PROFILE"):
    summary["perf_profile"] = os.environ["QA_PERF_PROFILE"]
if os.environ.get("QA_PERF_OUT"):
    summary["perf_out"] = os.environ["QA_PERF_OUT"]
if os.environ.get("QA_SOAK_MINUTES"):
    value = float(os.environ["QA_SOAK_MINUTES"])
    summary["soak_minutes"] = int(value) if value.is_integer() else value

gx = os.path.join(out, "unit_test.xml")
if os.path.isfile(gx):
    try:
        txt = open(gx, errors="replace").read()
        m = re.search(r'tests="(\d+)"[^>]*?failures="(\d+)"', txt)
        d = re.search(r'disabled="(\d+)"', txt)
        if m:
            summary["unit_test"] = {
                "tests": int(m.group(1)),
                "failures": int(m.group(2)),
                "disabled": int(d.group(1)) if d else None,
            }
    except Exception as e:
        summary["unit_test_parse_error"] = str(e)

summary["exit_codes"] = {}
for name in ("ut.log", "it.log", "perf.log", "soak.log"):
    p = os.path.join(out, name)
    if not os.path.isfile(p):
        continue
    txt = open(p, errors="replace").read()
    for m in re.finditer(r"^(\S+) exit=(\d+)$", txt, re.M):
        summary["exit_codes"][m.group(1)] = int(m.group(2))
    m = re.search(r"^=+ (.*(?:passed|failed|error).*) =+$", txt, re.M)
    if m:
        summary.setdefault("pytest", {})["summary_line"] = m.group(1)
    # list failing pytest node ids for the report
    fails = re.findall(r"^(FAILED|ERROR) (\S+)", txt, re.M)
    if fails:
        summary.setdefault("pytest", {})["failing"] = ["%s %s" % f for f in fails][:200]

with open(os.path.join(out, "summary.json"), "w") as f:
    json.dump(summary, f, indent=2, sort_keys=True)
print(json.dumps(summary, indent=2, sort_keys=True))
PY

echo "qa: test results in $OUT"

if [ "$DIAGNOSTIC" = "1" ] && { [ "$MODE" = "ut" ] || [ "$MODE" = "it" ] || [ "$MODE" = "all" ]; }; then
    echo "qa: DIAGNOSTIC mode - recording outcomes without failing the build"
    exit 0
fi
if [ "$FAILURES" -ne 0 ]; then
    echo "qa: TEST SUITES FAILED (see ${OUT}/summary.json)" >&2
    exit 1
fi
exit 0
