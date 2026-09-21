#!/usr/bin/env bash
# Series acceptance gate (REVIEW.md P2-8).
#
# Runs the series-focused gtest selection in the pinned compile container and
# FAILS nonzero on any test failure, crash, or missing output. This is the
# opposite policy from ci/phase0/run_tests.sh, which records upstream failures
# and always exits 0 (Phase 0 baseline policy, left intact).
#
# Usage:
#   ci/phase0/run_series_tests.sh                 # incremental build + gate
#   CLEAN=1 ci/phase0/run_series_tests.sh         # clean build + gate
#   SKIP_BUILD=1 ci/phase0/run_series_tests.sh    # gate only, reuse build/
#   GTEST_FILTER='TestSeries*' ci/phase0/run_series_tests.sh
#
# Output: phase0-results/series.log, series.xml, series-summary.json
# The summary records commit id, working-tree state, image digest, build
# flags, and test outputs together, so a green gate is reproducible evidence
# rather than a bare exit code.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/container.sh"

SKIP_BUILD="${SKIP_BUILD:-0}"
CLEAN="${CLEAN:-0}"
JOBS="${JOBS:-2}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

# Acceptance selection for the time-series work stream. Allowlist, not
# exclusion: anything outside this filter is some other stream's
# responsibility. Known upstream reds (e.g. TestCypherV2.TestProcedure, red
# for stale cacheStats/LMDB expectations and last-digit doubles before this
# work started) are excluded by construction; if a listed suite ever needs a
# temporary exception, add it to KNOWN_FAILURES below with a reason - never by
# deleting the suite from the filter.
GTEST_FILTER="${GTEST_FILTER:-TestSeries*:*Series*:TestSchema*:TestSchemaChange*:TestTransaction.*:TestLGraph.*:TestDetachProperty.*:TestQuery.TestCypherSuite:TestQuery.TestGqlSuite:TestCypherV2.TestFunction:TestCypherV2.TestExpression:TestCypherV2.TestQuery:TestCypherFieldData.*:TestCypherPlan.*}"
KNOWN_FAILURES="${KNOWN_FAILURES:-}"

RESULTS_DIR="${REPO_ROOT}/phase0-results"
OUT_LOG="${RESULTS_DIR}/series.log"
OUT_XML="${RESULTS_DIR}/series.xml"
OUT_SUMMARY="${RESULTS_DIR}/series-summary.json"
mkdir -p "${RESULTS_DIR}"

echo "series-gate: commit     $(git -C "${REPO_ROOT}" rev-parse HEAD)"
echo "series-gate: worktree   $(git -C "${REPO_ROOT}" status --short | wc -l) dirty files"
echo "series-gate: filter     ${GTEST_FILTER}"
echo "series-gate: exceptions [${KNOWN_FAILURES}]"

if [[ "${SKIP_BUILD}" != "1" ]]; then
    CLEAN="${CLEAN}" JOBS="${JOBS}" BUILD_TYPE="${BUILD_TYPE}" bash "${PHASE0_DIR}/build.sh"
fi

phase0_check_image "$PHASE0_COMPILE_IMAGE" --verify-id
IMAGE_ID="$(docker image inspect "$PHASE0_COMPILE_IMAGE" --format '{{.Id}}')"
echo "series-gate: image      ${PHASE0_COMPILE_IMAGE} ${IMAGE_ID}"

set +e
phase0_run_compile bash -lc "cd '${PHASE0_WORKDIR}/build/output' \
  && rm -rf testdb* .import_tmp \
  && ./unit_test --gtest_filter='${GTEST_FILTER}' --gtest_output=xml:'${PHASE0_WORKDIR}/phase0-results/series.xml' 2>&1; \
  echo SERIES_GATE_RC=\$?; rm -rf testdb* .import_tmp" 2>&1 | tee "$OUT_LOG"
PIPE_RC=${PIPESTATUS[0]}
set -e
GATE_RC="$(grep -o 'SERIES_GATE_RC=[0-9]*' "$OUT_LOG" | tail -n 1 | cut -d= -f2 || true)"

export SERIES_GATE_INFO="${REPO_ROOT}|${OUT_XML}|${OUT_LOG}|${GTEST_FILTER}|${KNOWN_FAILURES}|${IMAGE_ID}|${BUILD_TYPE}|${JOBS}|${GATE_RC}|${PIPE_RC}"
python3 - "${RESULTS_DIR}" <<'PY'
import json, os, re, subprocess, sys, datetime
out = sys.argv[1]
repo, xml, log, filt, known, image, build_type, jobs, gate_rc, pipe_rc = (
    os.environ["SERIES_GATE_INFO"].split("|"))
summary = {
    "gate": "series",
    "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "commit": subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo,
                             capture_output=True, text=True).stdout.strip(),
    "worktree": subprocess.run(["git", "status", "--short"], cwd=repo,
                               capture_output=True, text=True).stdout.strip().splitlines(),
    "image": image,
    "build_type": build_type,
    "jobs": jobs,
    "gtest_filter": filt,
    "known_failures": known.split() if known else [],
    "container_rc": int(pipe_rc) if pipe_rc else None,
    "unit_test_rc": int(gate_rc) if gate_rc else None,
}
if os.path.isfile(xml):
    try:
        txt = open(xml, errors="replace").read()
        m = re.search(r'tests="(\d+)"[^>]*?failures="(\d+)"', txt)
        d = re.search(r'disabled="(\d+)"', txt)
        errors = re.findall(r'errors="(\d+)"', txt)
        if m:
            summary["tests"] = int(m.group(1))
            summary["failures"] = int(m.group(2))
            summary["disabled"] = int(d.group(1)) if d else None
            summary["errors"] = int(errors[0]) if errors else 0
    except Exception as e:
        summary["xml_parse_error"] = str(e)
else:
    summary["xml_missing"] = True
txt = open(log, errors="replace").read() if os.path.isfile(log) else ""
failed = sorted(set(re.findall(r"\[  FAILED  \] (\S+\.\S+)", txt)))
summary["failed_tests"] = [f for f in failed if f != "TEST"]
summary["unexpected_failures"] = [
    f for f in summary["failed_tests"] if f not in summary["known_failures"]]
with open(os.path.join(out, "series-summary.json"), "w") as f:
    json.dump(summary, f, indent=2, sort_keys=True)
print(json.dumps(summary, indent=2, sort_keys=True))
PY

# Acceptance, all of it required:
# - the container ran and the test binary exited 0,
# - the XML result exists and parses with zero failures and zero errors,
# - every failing test (if any) is a listed known failure.
python3 - "${RESULTS_DIR}/series-summary.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
ok = True
for key in ("tests", "failures", "errors", "unit_test_rc", "container_rc"):
    if key not in s or s[key] is None:
        print(f"series-gate: REJECT: missing {key} (crash or missing output)")
        ok = False
if s.get("unit_test_rc", 1) != 0 or s.get("container_rc", 1) != 0:
    print("series-gate: REJECT: nonzero exit "
          f"(unit_test={s.get('unit_test_rc')} container={s.get('container_rc')})")
    ok = False
if s.get("failures", 1) != 0 or s.get("errors", 1) != 0:
    print(f"series-gate: REJECT: failures={s.get('failures')} errors={s.get('errors')}")
    ok = False
if s.get("tests", 0) <= 0:
    print("series-gate: REJECT: no tests ran (empty filter or missing XML)")
    ok = False
if s.get("unexpected_failures"):
    print(f"series-gate: REJECT: unexpected failures: {s['unexpected_failures']}")
    ok = False
if "xml_missing" in s or "xml_parse_error" in s:
    print("series-gate: REJECT: result XML missing or unparsable")
    ok = False
print(f"series-gate: {'ACCEPT' if ok else 'REJECT'}: "
      f"{s.get('tests', '?')} tests, {s.get('failures', '?')} failures, "
      f"{len(s.get('known_failures', []))} known failures excused")
sys.exit(0 if ok else 1)
PY
