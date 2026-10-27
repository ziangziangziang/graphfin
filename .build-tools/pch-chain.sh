#!/bin/bash
# Build the server-library precompiled-header chain.
#
# clang does not re-serialize a PCH loaded via -include-pch while building
# another PCH, so the closure is precompiled in stages: each stage only pays
# for the headers it adds on top of its parent.  That is what makes a ~1.1 GB
# header closure fit on a host with ~800 MB of usable RAM.
#
# The chain is split into families so a translation unit only materialises the
# declarations it actually needs.  A single "everything" PCH costs ~130 MB
# more per translation unit than the right family PCH, which is the difference
# between fitting in RAM and getting OOM-killed.
#
# The levels are deliberately fine-grained: a level's peak is
# "materialise the parent + parse the new headers", so a long chain of small
# levels has a lower worst case than one big level.
set -uo pipefail
R=/home/pdfexe/graphfin
B=$R/build
L=$R/.build-logs
source "$R/.build-tools/flags.sh"
mkdir -p "$L"

# --- family definitions -------------------------------------------------
printf '#include "tools/lgraph_log.h"\n' > "$B/pch_base.h"

cat > "$B/pch_l1.h" <<'EOF'
#include "db/db.h"
#include "core/lightning_graph.h"
#include "db/galaxy.h"
EOF

cat > "$B/pch_l2.h" <<'EOF'
#include "server/state_machine.h"
#include "core/blob_manager.h"
EOF

cat > "$B/pch_l3.h" <<'EOF'
#include "bolt_raft/raft_driver.h"
#include "bolt_raft/bolt_raft.pb.h"
#include "protobuf/ha.pb.h"
EOF

cat > "$B/pch_l4.h" <<'EOF'
#include "server/proto_convert.h"
#include "cypher/resultset/record.h"
#include "execution_plan/runtime_context.h"
EOF

cat > "$B/pch_l5.h" <<'EOF'
#include "cypher/execution_plan/scheduler.h"
EOF

# Same closure, but as a single flat level (no parent PCH).  clang 17 segfaults
# in ASTReader::loadDeclUpdateRecords when it *loads* the chained
# pch_l5.h.pch, so the server TUs that need the scheduler use this one instead.
# Peak while building it is ~940 MB.
cat > "$B/pch_l5b.h" <<'EOF'
#include "server/state_machine.h"
#include "cypher/execution_plan/scheduler.h"
EOF

cat > "$B/pch_bolt.h" <<'EOF'
#include "bolt/connection.h"
#include "server/bolt_session.h"
#include "server/bolt_server.h"
#include "server/bolt_raft_server.h"
EOF

cat > "$B/pch_import.h" <<'EOF'
#include "import/import_v2.h"
#include "import/import_v3.h"
#include "import/import_online.h"
#include "import/blob_writer.h"
#include "import/import_utils.h"
#include "import/dense_string.h"
#include "import/import_config_parser.h"
#include "import/file_cutter.h"
#include "import/parse_delimiter.h"
EOF

cat > "$B/pch_http.h" <<'EOF'
#include "lgraph/lgraph_exceptions.h"
#include "http/http_server.h"
#include "http/http_utils.h"
#include "http/import_task.h"
#include "http/algo_task.h"
#include "http/import_manager.h"
#include "import/import_config_parser.h"
#include "import/file_cutter.h"
#include "import/parse_delimiter.h"
EOF

cat > "$B/pch_rest.h" <<'EOF'
#include "lgraph/lgraph_exceptions.h"
#include "restful/server/rest_server.h"
#include "restful/server/json_convert.h"
#include "server/json_convert.h"
#include "server/db_management_client.h"
EOF

cat > "$B/pch_srv.h" <<'EOF'
#include "lgraph/lgraph_exceptions.h"
#include "bolt/connection.h"
#include "server/bolt_server.h"
#include "server/bolt_raft_server.h"
#include "http/http_server.h"
#include "http/import_manager.h"
#include "restful/server/rest_server.h"
#include "restful/server/json_convert.h"
#include "server/json_convert.h"
#include "server/db_management_client.h"
#include "server/lgraph_server.h"
EOF

printf '#include "server/db_management_client.h"\n' > "$B/pch_light.h"
cat > "$B/pch_http2.h" <<'EOF'
#include "lgraph/lgraph_exceptions.h"
#include "http/http_utils.h"
#include "http/import_manager.h"
EOF

# --- build --------------------------------------------------------------
build() {
  local tag="$1" hdr="$2" out="$3" parent="$4"
  local extra=()
  [ -n "$parent" ] && extra=(-include-pch "$parent")
  cd "$B/src" || return 1
  if [ "${FORCE:-0}" != "1" ] && [ -f "$out" ] && [ "$out" -nt "$hdr" ]; then
    echo "PCH_$tag rc=0 size=$(stat -c%s "$out") (reused)" | tee -a "$L/retry.log"
    return 0
  fi
  local attempt rc
  for attempt in 1 2 3; do
    /usr/bin/time -q -f "PCH_$tag\t%e\t%M\t%C" -a -o "$L/tu-metrics.tsv" \
      "$CLANG" "${CLANG_COMMON[@]}" "${PCH_FLAGS[@]}" "${extra[@]}" \
      -x c++-header "$hdr" -o "$out"
    rc=$?
    [ $rc -eq 0 ] && break
    echo "PCH-RETRY $tag attempt=$attempt rc=$rc" | tee -a "$L/retry.log"
    evict_cache; sync; sleep 15
  done
  echo "PCH_$tag rc=$rc size=$(stat -c%s "$out" 2>/dev/null || echo 0)" | tee -a "$L/retry.log"
  return $rc
}

if [ "${FORCE:-0}" = "1" ]; then
  rm -f "$B"/pch_base.h.pch "$B"/pch_l1.h.pch "$B"/pch_l2.h.pch "$B"/pch_l3.h.pch \
        "$B"/pch_l4.h.pch "$B"/pch_l5.h.pch "$B"/pch_l5b.h.pch "$B"/pch_bolt.h.pch \
        "$B"/pch_import.h.pch \
        "$B"/pch_http.h.pch "$B"/pch_rest.h.pch "$B"/pch_srv.h.pch \
        "$B"/pch_light.h.pch "$B"/pch_http2.h.pch
fi

build BASE   "$B/pch_base.h"   "$B/pch_base.h.pch"   ""                    || exit 1
build L1     "$B/pch_l1.h"     "$B/pch_l1.h.pch"     "$B/pch_base.h.pch"   || exit 1
build L2     "$B/pch_l2.h"     "$B/pch_l2.h.pch"     "$B/pch_l1.h.pch"     || exit 1
build L3     "$B/pch_l3.h"     "$B/pch_l3.h.pch"     "$B/pch_l2.h.pch"     || exit 1
build L4     "$B/pch_l4.h"     "$B/pch_l4.h.pch"     "$B/pch_l3.h.pch"     || exit 1
build L5     "$B/pch_l5.h"     "$B/pch_l5.h.pch"     "$B/pch_l4.h.pch"     || exit 1
build L5B    "$B/pch_l5b.h"    "$B/pch_l5b.h.pch"    ""                    || exit 1
build LIGHT  "$B/pch_light.h"  "$B/pch_light.h.pch"  "$B/pch_base.h.pch"   || exit 1
build HTTP2  "$B/pch_http2.h"  "$B/pch_http2.h.pch"  "$B/pch_base.h.pch"   || exit 1
build BOLT   "$B/pch_bolt.h"   "$B/pch_bolt.h.pch"   "$B/pch_l4.h.pch"     || exit 1
build IMPORT "$B/pch_import.h" "$B/pch_import.h.pch" "$B/pch_l1.h.pch"     || exit 1
build HTTP   "$B/pch_http.h"   "$B/pch_http.h.pch"   "$B/pch_l4.h.pch"     || exit 1
build REST   "$B/pch_rest.h"   "$B/pch_rest.h.pch"   "$B/pch_l4.h.pch"     || exit 1
build SRV    "$B/pch_srv.h"    "$B/pch_srv.h.pch"    "$B/pch_l4.h.pch"     || exit 1
echo "PCH CHAIN COMPLETE"
