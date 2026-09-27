#!/bin/bash
# PR gate phase 2 of 2: server-alias smoke check + targeted unit/integration.
# Requires phase 1 (ci/github_pr_build.sh) to have run in the same tree.
# All result XML lands under $WORKSPACE/testresult so CI can mount that
# directory out of the container and upload it as evidence, pass or fail.
set -euo pipefail

cd "$WORKSPACE"

echo "=== PR: server alias smoke check ==="
# Tests the alias the build itself generated
# (src/BuildLGraphServer.cmake POST_BUILD into build/output). It is created
# by hand nowhere: if the CMake rule disappears, this check fails instead of
# passing on a staged copy. Full `make install` layout stays a separate,
# explicitly qualified concern.
bash "$WORKSPACE/ci/release/check_server_alias.sh" "$WORKSPACE/build/output"

echo "=== PR: targeted unit ==="
mkdir -p "$WORKSPACE/testresult/gtest/"
cd "$WORKSPACE/build/output"
if [ ! -e data ] && [ -d ../../test/integration/data ]; then
  ln -sfn ../../test/integration/data data
fi
export LD_LIBRARY_PATH="/usr/local/lib64/lgraph:/usr/local/lib64:/usr/local/lib:${WORKSPACE}/build/output:${LD_LIBRARY_PATH:-}"
export OMP_NUM_THREADS=2

./fma_unit_test -t all
# Selection from test/suites.json (suite pr-unit); suites.py validates and
# fails on an empty filter so a catalog typo can never run zero tests.
PR_GTEST_FILTER="$(python3 "$WORKSPACE/test/suites.py" --suite pr-unit --field gtest_filter)"
./unit_test \
  --gtest_filter="$PR_GTEST_FILTER" \
  --gtest_output=xml:"$WORKSPACE/testresult/gtest/pr-unit.xml"
# Gate the XML against the suite contract, not just "n > 0": a green badge must
# not be reachable by running less than the catalog asks for. This rejects a
# zero/partial selection, any failure, any skip, a below-floor case count, a
# catalog-listed test that silently stopped running, an unmatched wildcard
# pattern, and a mandatory family with zero executed cases, and requires the
# built binaries to exist.
python3 "$WORKSPACE/test/check_results.py" \
  --suite pr-unit \
  --xml "$WORKSPACE/testresult/gtest/pr-unit.xml" \
  --artifacts "$WORKSPACE/build/output"
rm -rf testdb* .import_tmp 2>/dev/null || true

echo "=== PR: targeted integration ==="
cp -f ../../src/client/python/TuGraphClient/TuGraphClient.py .
cp -f ../../src/client/python/TuGraphClient/TuGraphRestClient.py .
cp -rf ../../test/integration/* ./
export PYTHONPATH="${WORKSPACE}/build/output:${WORKSPACE}/src/client/python/TuGraphClient:${PYTHONPATH:-}"
export PYTHONDONTWRITEBYTECODE=1

# Fast subset: merge gate + phase0 multi-graph/lifecycle files (~19s measured).
python3 -m pytest -v \
  test_merge_series.py \
  test_multi_graph_lifecycle.py \
  test_multi_graph_crud.py \
  test_multi_graph_acl.py \
  test_restart_and_failure_recovery.py \
  test_backup_restore_scale.py \
  test_graph_ceiling_regression.py \
  test_graph_lifecycle_eviction.py \
  test_graph_lifecycle_restart.py \
  --junitxml="$WORKSPACE/testresult/pr-integration.xml"

echo "=== PR: done ==="
