#!/bin/bash
# PR gate phase 1 of 2: lint + targeted build only. No tests run here.
# Phase 2 (ci/github_pr_test.sh) runs separately so the CI job can mount a
# results directory and export test evidence even when tests fail -- results
# produced inside a `docker build` step are unreachable after the build ends.
# ci/github_pr.sh runs both phases back to back for local/dev use.
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

echo "=== PR: build web frontends ==="
# Generates the browser-resource tree consumed by the install step. This is
# NOT the C++ toolchain: compilers and third-party C++ libraries come from
# the compile image. Skipping it breaks `make install` packaging, not the
# compile itself.
(cd deps && bash ./build_deps.sh -j2)

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

# The lgraph_server target's POST_BUILD step (src/BuildLGraphServer.cmake)
# creates build/output/graphfin_server next to lgraph_server. Phase 2 tests
# that generated alias directly; nothing is staged by hand.
echo "=== PR: build done ==="
