#!/usr/bin/env bash
# Phase 0: test execution, run INSIDE the compile container.
#
# Invoked by ci/phase0/run_tests.sh. Reads:
#   PHASE0_TEST_MODE   ut | it | all
#   PHASE0_TEST_FILES  optional space-separated pytest file list (it mode)
#
# Failures PROPAGATE to CI by default: the script exits non-zero if any test
# suite fails. Set PHASE0_TEST_DIAGNOSTIC=1 to keep the historical
# "record-but-don't-fail" behaviour (useful when capturing a known-flaky
# baseline); in that mode results from successive runs are kept, otherwise each
# run starts from a clean results directory so outcomes are never mixed across
# runs.

set -uo pipefail

REPO="${PHASE0_WORKDIR:-/workspace}"
OUT="${REPO}/phase0-results"
MODE="${PHASE0_TEST_MODE:-all}"
DIAGNOSTIC="${PHASE0_TEST_DIAGNOSTIC:-0}"
mkdir -p "$OUT"

# Isolate results by run: wipe the machine-readable outputs unless explicitly
# asked to keep history. Without this a stale unit_test.xml/ut.log from a
# previous run leaks into an unrelated mode's summary (observed: an `it` run
# reporting a unit_test exit code).
if [ "$DIAGNOSTIC" != "1" ]; then
    rm -f "$OUT"/ut.log "$OUT"/it.log "$OUT"/unit_test.xml "$OUT"/summary.json
fi

FAILURES=0
mark_fail() { FAILURES=1; }

cd "${REPO}/build/output" || exit 3
if [ ! -e data ] && [ -d ../../test/integration/data ]; then
    ln -sfn ../../test/integration/data data
fi
export LD_LIBRARY_PATH="/usr/local/lib64/lgraph:/usr/local/lib64:/usr/local/lib:${REPO}/build/output:${LD_LIBRARY_PATH:-}"
export OMP_NUM_THREADS=2

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

    # Stage the test tree the way ci/github_ci.sh does.
    cp -r "${REPO}"/test/integration/* . 2>/dev/null
    cp -r "${REPO}"/src/client/python/TuGraphClient/*.py . 2>/dev/null
    mkdir -p learn/examples 2>/dev/null
    cp -r "${REPO}"/learn/examples/* learn/examples/ 2>/dev/null
    # github_ci.sh copies demo/movie into cwd; test_http_server.py reads
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
    rm -f test_algo.py test_algo_v2.py test_sampling.py test_train.py

    if [ -n "${PHASE0_TEST_FILES:-}" ]; then
        # PHASE0_TEST_FILES is comma-separated because the value travels through
        # `docker run -e` via a whitespace-split variable list.
        FILES="$(echo "${PHASE0_TEST_FILES}" | tr ',' ' ')"
        # shellcheck disable=SC2086
        python3 -m pytest ${FILES} -v -p no:cacheprovider >>"$OUT/it.log" 2>&1
    else
        python3 -m pytest -v -p no:cacheprovider >>"$OUT/it.log" 2>&1
    fi
    local rc=$?
    echo "pytest exit=$rc" | tee -a "$OUT/it.log"
    return "$rc"
}

case "$MODE" in
    ut)  run_ut || mark_fail ;;
    it)  run_it || mark_fail ;;
    all) run_ut || mark_fail; run_it || mark_fail ;;
    *)   echo "unknown PHASE0_TEST_MODE=$MODE" >&2; exit 2 ;;
esac

# --- machine-readable summary ---------------------------------------------
python3 - "$OUT" <<'PY'
import json, os, re, sys, time
out = sys.argv[1]
summary = {
    "mode": os.environ.get("PHASE0_TEST_MODE", "?"),
    "run_id": os.environ.get("PHASE0_RUN_ID", ""),
    "git_commit": os.environ.get("PHASE0_GIT_COMMIT", ""),
    "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
    "diagnostic": os.environ.get("PHASE0_TEST_DIAGNOSTIC", "0") == "1",
}

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
for name in ("ut.log", "it.log"):
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

echo "phase0: test results in $OUT"

if [ "$DIAGNOSTIC" = "1" ]; then
    echo "phase0: DIAGNOSTIC mode - recording outcomes without failing the build"
    exit 0
fi
if [ "$FAILURES" -ne 0 ]; then
    echo "phase0: TEST SUITES FAILED (see ${OUT}/summary.json)" >&2
    exit 1
fi
exit 0
