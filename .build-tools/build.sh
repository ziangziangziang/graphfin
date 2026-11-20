#!/bin/bash
# Serial, memory-limited build of lgraph_server with sampling.
#
# Reproducible entry point for the low-memory host build. Requires:
#   * deps/install populated by deps/build_deps.sh
#   * OpenBLAS under deps/install/OpenBLAS (or -DLGRAPH_OPENBLAS_ROOT)
#   * an LLVM 17 tree at .clangpkg (override with LGRAPH_CLANG_ROOT)
set -uo pipefail
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
source "$R/.build-tools/env.sh"
B=${BUILD_DIR:-$R/build}
L=${LGRAPH_BUILD_LOGS:-$R/.build-logs}
mkdir -p "$B" "$L"

if [ ! -f "$B/CMakeCache.txt" ]; then
  echo "configuring $B"
  cmake -S "$R" -B "$B" \
    -DCMAKE_BUILD_TYPE=Release \
    -DWITH_TESTS=OFF \
    -DBUILD_PROCEDURE=OFF \
    -DLGRAPH_STATIC_RUNTIME=OFF \
    -DCMAKE_C_COMPILER=/usr/bin/gcc \
    -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
    -DCMAKE_C_COMPILER_LAUNCHER="$R/.build-tools/cc-metrics.sh" \
    -DCMAKE_CXX_COMPILER_LAUNCHER="$R/.build-tools/cc-metrics.sh" || exit 1
fi

"$R/.build-tools/pch-chain.sh" || exit 1
"$R/.build-tools/pch-cypher.sh" || exit 1

cd "$B" || exit 1
rm -f "$L/monitor.stop"
rm -f "$L/tu-metrics.tsv"
START=$(date +%s)
echo "$START" > "$L/build.start"
python3 "$R/.build-tools/monitor.py" "$L/mem.csv" "$L/monitor.stop" 2 >/dev/null 2>&1 &
MON=$!
# Make the build the first OOM victim, not the IDE/session.
echo 1000 > /proc/self/oom_score_adj 2>/dev/null || true
echo "build start: $(date -u +%FT%TZ)" | tee "$L/build.log"
nice -n 19 make -j1 lgraph_server 2>&1 | tee -a "$L/build.log"
RC=${PIPESTATUS[0]}
END=$(date +%s)
echo "build end: $(date -u +%FT%TZ) rc=$RC elapsed_s=$((END-START))" | tee -a "$L/build.log"
touch "$L/monitor.stop"
wait "$MON" 2>/dev/null
exit "$RC"
