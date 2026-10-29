#!/bin/bash
# Rebuild the precompiled header used by the cypher library.
#
# The cypher target is compiled with a different flag set than the server
# library (no -fopenmp, no include-shim, different include order), so it
# cannot share the server PCH chain.  Its PCH is built from op.h, which pulls
# the whole cypher operator closure.
set -uo pipefail
R=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
B=${BUILD_DIR:-$R/build}
L=${LGRAPH_BUILD_LOGS:-$R/.build-logs}
CLANGDIR=${LGRAPH_CLANG_ROOT:-$R/.clangpkg}
export LD_LIBRARY_PATH=$CLANGDIR/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

FLAGS_MAKE=$B/src/CMakeFiles/lgraph_cypher_lib.dir/flags.make
PCH_HASH=$(cat "$0" "$FLAGS_MAKE" "$R/src/cypher/execution_plan/ops/op.h" 2>/dev/null \
           | sha256sum | cut -d' ' -f1)
if [ "${FORCE:-0}" != "1" ] && [ -f "$B/op_pch.h.pch" ] \
   && [ -f "$L/op-pch.stamp" ] \
   && [ "$(cat "$L/op-pch.stamp")" = "$PCH_HASH" ]; then
  echo "cypher PCH up to date ($PCH_HASH)"
  exit 0
fi

GCCDIR=${GCCDIR:-$(gcc -print-file-name=include 2>/dev/null | sed 's#/include$##')}
PYTHON_INCLUDE=${PYTHON_INCLUDE:-/usr/include/python3.6m}

DEFS="-DBOOST_STACKTRACE_USE_ADDR2LINE=1 -DFOLLY_ASSUME_NO_TCMALLOC -DFOLLY_USE_JEMALLOC -DUSE_JEMALLOC -DUSING_CENTOS9=0"
INCS="-I$R/src/cypher -I$PYTHON_INCLUDE -I$R/deps/geax-front-end/include -I$R/include -I$R/src -I$R/deps -isystem $R/deps/install/include -isystem $R/deps/install/include/antlr4-runtime"
FLAGS="-DLGRAPH_USE_MOCK_KV=0 -DOPENSSL_NO_KRB5=1 -fmacro-prefix-map=$R/= -D__STDC_FORMAT_MACROS -std=gnu++17 -O0 -g0 -DNDEBUG -pipe -DGFLAGS_NS= -D__const__= -fPIC -fno-omit-frame-pointer -w"
GCCINST="--gcc-install-dir=$GCCDIR -I$GCCDIR/include"

mkdir -p "$L"
printf '#include "cypher/execution_plan/ops/op.h"\n' > "$B/op_pch.h"

cd "$B/src" || exit 1
/usr/bin/time -q -f 'PCHOP\t%e\t%M\t%C' -a -o "$L/tu-metrics.tsv" \
  "$CLANGDIR/usr/bin/clang++" $DEFS $INCS $FLAGS $GCCINST \
  -x c++-header "$B/op_pch.h" -o "$B/op_pch.h.pch"
rc=$?
echo "PCHOP rc=$rc size=$(stat -c%s "$B/op_pch.h.pch" 2>/dev/null || echo 0)" | tee -a "$L/retry.log"
if [ $rc -eq 0 ]; then
  printf '%s\n' "$PCH_HASH" > "$L/op-pch.stamp"
fi
exit $rc
