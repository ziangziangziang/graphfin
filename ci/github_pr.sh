#!/bin/bash
# Fast PR path: lint + targeted build/tests only.
# Full suite (asan, coverage, sphinx docs, web UI, full ut/it) remains in
# ci/github_ci.sh (driven by BUILD_TYPE/TEST_TYPE from the main-push jobs).
set -euo pipefail

cd "$WORKSPACE"

echo "=== PR: ccache ==="
if command -v ccache >/dev/null 2>&1; then
  ccache -M 2G
  ccache -z
  export CMAKE_C_COMPILER_LAUNCHER=ccache
  export CMAKE_CXX_COMPILER_LAUNCHER=ccache
  ccache --version | head -1
else
  echo "ccache not installed; building without cache"
fi

echo "=== PR: cpplint ==="
bash ./cpplint/check_all.sh

echo "=== PR: docs/product checks ==="
python3 ci/release/check_docs.py

echo "=== PR: configure (RelWithDebInfo, no procedures, no coverage) ==="
mkdir -p build
cd build
# -j2 is the memory ceiling on 7–8 GiB runners (cypher TUs need 1.5–2.5 GiB).
cmake .. \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_PROCEDURE=OFF \
  -DWITH_TESTS=ON \
  -DCMAKE_C_COMPILER_LAUNCHER="${CMAKE_C_COMPILER_LAUNCHER:-}" \
  -DCMAKE_CXX_COMPILER_LAUNCHER="${CMAKE_CXX_COMPILER_LAUNCHER:-}"

echo "=== PR: build targeted binaries ==="
make -j2 unit_test fma_unit_test lgraph_server lgraph_client_python

if command -v ccache >/dev/null 2>&1; then
  echo "=== PR: ccache stats ==="
  ccache -s
fi

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
python3 - "$WORKSPACE/testresult/gtest/pr-unit.xml" <<'PY'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
suites = root.findall("testsuite") or ([root] if root.tag == "testsuite" else [])
n = sum(int(s.get("tests", 0)) for s in suites)
print("pr-unit: %d tests selected" % n)
sys.exit(0 if n > 0 else 1)
PY
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
