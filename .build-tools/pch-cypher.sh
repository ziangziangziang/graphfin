#!/bin/bash
# Rebuild the precompiled header used by the cypher library.
#
# The cypher target is compiled with a different flag set than the server
# library (no -fopenmp, no include-shim, different include order), so it
# cannot share the server PCH chain.  Its PCH is built from op.h, which pulls
# the whole cypher operator closure.
set -uo pipefail
R=/home/pdfexe/graphfin
B=$R/build
L=$R/.build-logs
export LD_LIBRARY_PATH=$R/.clangpkg/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

DEFS="-DBOOST_STACKTRACE_USE_ADDR2LINE=1 -DFOLLY_ASSUME_NO_TCMALLOC -DFOLLY_USE_JEMALLOC -DUSE_JEMALLOC -DUSING_CENTOS9=0"
INCS="-I$R/src/cypher -I/usr/include/python3.6m -I$R/deps/geax-front-end/include -I$R/include -I$R/src -I$R/deps -isystem $R/deps/install/include -isystem $R/deps/install/include/antlr4-runtime"
FLAGS="-DLGRAPH_USE_MOCK_KV=0 -DOPENSSL_NO_KRB5=1 -fmacro-prefix-map=$R/= -D__STDC_FORMAT_MACROS -std=gnu++17 -O0 -g0 -DNDEBUG -pipe -DGFLAGS_NS= -D__const__= -fPIC -fno-omit-frame-pointer -w"
GCCINST="--gcc-install-dir=/usr/lib/gcc/x86_64-redhat-linux/10 -I/usr/lib/gcc/x86_64-redhat-linux/10/include"

printf '#include "cypher/execution_plan/ops/op.h"\n' > /tmp/op_pch.h

cd "$B/src" || exit 1
/usr/bin/time -q -f 'PCHOP\t%e\t%M\t%C' -a -o "$L/tu-metrics.tsv" \
  "$R/.clangpkg/usr/bin/clang++" $DEFS $INCS $FLAGS $GCCINST \
  -x c++-header /tmp/op_pch.h -o "$B/op_pch.h.pch"
rc=$?
echo "PCHOP rc=$rc size=$(stat -c%s "$B/op_pch.h.pch" 2>/dev/null || echo 0)" | tee -a "$L/retry.log"
exit $rc
