#!/usr/bin/env bash
# Series acceptance gate (REVIEW.md P2-8).
#
# Runs the series-focused gtest selection in the pinned compile container and
# FAILS nonzero on any test failure, crash, or missing output. This is the
# opposite policy from dev/phase0/run_tests.sh, which records upstream failures
# and always exits 0 (Phase 0 baseline policy, left intact).
#
# Usage:
#   dev/phase0/run_series_tests.sh                 # incremental build + gate
#   CLEAN=1 dev/phase0/run_series_tests.sh         # clean build + gate
#   SKIP_BUILD=1 dev/phase0/run_series_tests.sh    # gate only, reuse build/
#   GTEST_FILTER='TestSeries*' dev/phase0/run_series_tests.sh
#
# Output: phase0-results/series.log, series-<stamp>.xml (+ series-latest.xml
# copy), series-summary.json. The summary records commit id, source-diff and
# untracked-content hashes, binary hashes, image digest, actual CMake options,
# and parsed test outputs together, so a green gate is reproducible evidence
# rather than a bare exit code.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/container.sh"

SKIP_BUILD="${SKIP_BUILD:-0}"
CLEAN="${CLEAN:-0}"
JOBS="${JOBS:-2}"
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"

# Acceptance selection from test/suites.json (suite series-gate). Allowlist, not
# exclusion: anything outside this filter is some other stream's
# responsibility. Known upstream reds (e.g. TestCypherV2.TestProcedure, red
# for stale cacheStats/LMDB expectations and last-digit doubles before this
# work started) are excluded by construction; if a listed suite ever needs a
# temporary exception, add it to KNOWN_FAILURES below with a reason - never by
# deleting the suite from the filter. suites.py fails on an empty filter.
if [[ -z "${GTEST_FILTER:-}" ]]; then
    GTEST_FILTER="$(python3 "${REPO_ROOT}/test/suites.py" --suite series-gate --field gtest_filter)"
fi
KNOWN_FAILURES="${KNOWN_FAILURES:-}"

RESULTS_DIR="${REPO_ROOT}/phase0-results"
OUT_LOG="${RESULTS_DIR}/series.log"
RUN_STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_XML="${RESULTS_DIR}/series-${RUN_STAMP}.xml"
OUT_SUMMARY="${RESULTS_DIR}/series-summary.json"
mkdir -p "${RESULTS_DIR}"
# A stale XML from an earlier run must never be mistaken for this run's
# evidence: each run writes a fresh timestamped path (also copied to
# series-latest.xml for convenience).
rm -f "${RESULTS_DIR}/series.xml"
GATE_START_EPOCH="$(date +%s)"

echo "series-gate: commit     $(git -C "${REPO_ROOT}" rev-parse HEAD)"
echo "series-gate: worktree   $(git -C "${REPO_ROOT}" status --short | wc -l) dirty files"
echo "series-gate: filter     ${GTEST_FILTER}"
echo "series-gate: exceptions [${KNOWN_FAILURES}]"
echo "series-gate: xml        ${OUT_XML}"

if [[ "${SKIP_BUILD}" != "1" ]]; then
    CLEAN="${CLEAN}" JOBS="${JOBS}" BUILD_TYPE="${BUILD_TYPE}" bash "${PHASE0_DIR}/build.sh"
fi

phase0_check_image "$PHASE0_COMPILE_IMAGE" --verify-id
IMAGE_ID="$(docker image inspect "$PHASE0_COMPILE_IMAGE" --format '{{.Id}}')"
echo "series-gate: image      ${PHASE0_COMPILE_IMAGE} ${IMAGE_ID}"

set +e
phase0_run_compile bash -lc "cd '${PHASE0_WORKDIR}/build/output' \
  && rm -rf testdb* .import_tmp \
  && ./unit_test --gtest_filter='${GTEST_FILTER}' --gtest_output=xml:'${PHASE0_WORKDIR}/phase0-results/series-${RUN_STAMP}.xml' 2>&1; \
  echo SERIES_GATE_RC=\$?; rm -rf testdb* .import_tmp" 2>&1 | tee "$OUT_LOG"
PIPE_RC=${PIPESTATUS[0]}
set -e
GATE_RC="$(grep -o 'SERIES_GATE_RC=[0-9]*' "$OUT_LOG" | tail -n 1 | cut -d= -f2 || true)"
if [[ -f "${OUT_XML}" ]]; then
    cp -f "${OUT_XML}" "${RESULTS_DIR}/series-latest.xml"
fi

export SERIES_GATE_INFO="${REPO_ROOT}|${OUT_XML}|${OUT_LOG}|${GTEST_FILTER}|${KNOWN_FAILURES}|${IMAGE_ID}|${BUILD_TYPE}|${JOBS}|${GATE_RC}|${PIPE_RC}|${SKIP_BUILD}|${CLEAN}|${GATE_START_EPOCH}"
python3 - "${RESULTS_DIR}" <<'PY'
import hashlib
import json
import os
import re
import subprocess
import sys
import datetime
import xml.etree.ElementTree as ET
out = sys.argv[1]
(repo, xml, log, filt, known, image, build_type, jobs, gate_rc, pipe_rc,
 skip_build, clean, start_epoch) = os.environ["SERIES_GATE_INFO"].split("|")


def run_git(args):
    return subprocess.run(["git", "-C", repo] + args, capture_output=True,
                          text=True).stdout.strip()


def sha256_file(path, cap_mb=2048):
    try:
        if os.path.getsize(path) > cap_mb * 1024 * 1024:
            return "too_large_skipped"
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError as e:
        return "unreadable:%s" % e


summary = {
    "gate": "series",
    "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "commit": run_git(["rev-parse", "HEAD"]),
    "worktree": run_git(["status", "--short"]).splitlines(),
    # Dirty-build provenance: the exact source diff plus hashes of every
    # untracked file, so the tested content is reconstructible.
    "source_diff_sha256": hashlib.sha256(
        run_git(["diff"]).encode()).hexdigest(),
    "untracked": [],
    "image": image,
    "build_type": build_type,
    "build_skipped": skip_build == "1",
    "clean_build": clean == "1",
    "jobs": jobs,
    "gtest_filter": filt,
    "known_failures": known.split() if known else [],
    "container_rc": int(pipe_rc) if pipe_rc else None,
    "unit_test_rc": int(gate_rc) if gate_rc else None,
}
for path in run_git(["ls-files", "--others", "--exclude-standard"]).splitlines()[:200]:
    full = os.path.join(repo, path)
    summary["untracked"].append(
        {"path": path, "sha256": sha256_file(full, cap_mb=10)})
# Actual build configuration and tested binaries.
cmake_cache = os.path.join(repo, "build", "CMakeCache.txt")
summary["cmake_options"] = {}
if os.path.isfile(cmake_cache):
    for line in open(cmake_cache, errors="replace"):
        for key in ("CMAKE_BUILD_TYPE:", "ENABLE_ASAN:", "ENABLE_TSAN:",
                    "ENABLE_UBSAN:", "WITH_TESTS:", "BUILD_PROCEDURE:",
                    "ENABLE_BUILD_ON_AARCH64:", "CMAKE_CXX_COMPILER:"):
            if line.startswith(key):
                summary["cmake_options"][key[:-1]] = line.strip().split("=", 1)[1]
summary["binaries"] = {
    name: sha256_file(os.path.join(repo, "build", "output", name))
    for name in ("unit_test", "liblgraph.so") + tuple(
        f for f in os.listdir(os.path.join(repo, "build", "output"))
        if f.startswith("liblgraph.so.")) if os.path.isfile(
            os.path.join(repo, "build", "output", name))
}
# Result XML: parsed with a real parser, checked for freshness (written by
# this run, not a stale file) and completeness (every reported test has a
# testcase element).
if os.path.isfile(xml):
    try:
        summary["xml_mtime_utc"] = datetime.datetime.fromtimestamp(
            os.path.getmtime(xml), datetime.timezone.utc).isoformat()
        summary["xml_fresh"] = os.path.getmtime(xml) >= int(start_epoch)
        root = ET.parse(xml).getroot()
        suites = root.findall("testsuite")
        if not suites and root.tag == "testsuite":
            suites = [root]
        cases = [c for s in suites for c in s.findall("testcase")]
        summary["tests"] = sum(int(s.get("tests", 0)) for s in suites)
        summary["failures"] = sum(int(s.get("failures", 0)) for s in suites)
        summary["disabled"] = sum(int(s.get("disabled", 0)) for s in suites)
        summary["errors"] = sum(int(s.get("errors", 0)) for s in suites)
        summary["testcase_elements"] = len(cases)
        summary["xml_complete"] = (
            len(cases) == summary["tests"] and summary["tests"] > 0)
        summary["xml_suites"] = [s.get("name") for s in suites]
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
# - the XML result exists, is fresh to this run, parses, and is complete,
#   with zero failures and zero errors,
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
if not s.get("xml_fresh"):
    print("series-gate: REJECT: result XML is stale (predates this run)")
    ok = False
if not s.get("xml_complete"):
    print("series-gate: REJECT: result XML incomplete "
          f"(testcases={s.get('testcase_elements')} tests={s.get('tests')})")
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
