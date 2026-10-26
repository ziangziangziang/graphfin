#!/bin/bash
# Compiler launcher for the memory-constrained build.
#
# Measured facts on this host (1871 MB RAM, 1024 MB swap file, and a cgroup
# whose memory.swappiness=0 so the kernel never swaps anonymous pages on its
# own):
#   * GCC and clang peak at the same ~1.13 GB on the heavy server TUs.  The
#     peak is dominated by the *header closure*, not by code generation.
#   * Precompiling the closure in stages (see pch-chain.sh) brings the peak
#     down to ~880 MB, which fits.  Using a single "everything" PCH instead
#     of a per-family one costs ~130 MB more per translation unit and does
#     not fit.
#   * The swap file is a *file* on ext4, so MADV_PAGEOUT moves anonymous
#     pages into the page cache (SwapCached) instead of freeing RAM: swapping
#     is RAM-neutral on this host and cannot be used to create headroom.
#   * The cypher library is prebuilt with its own clang PCH (op.h); keep
#     using it and keep its flag combination untouched.
L=/home/pdfexe/graphfin/.build-logs
R=/home/pdfexe/graphfin
B=$R/build
CLANG=$R/.clangpkg/usr/bin/clang++
CYPHER_PCH=$B/op_pch.h.pch
SHIM=$R/.clangpkg/include-shim
GCCDIR=/usr/lib/gcc/x86_64-redhat-linux/10
TU_TIMEOUT=${TU_TIMEOUT:-2400}
ATTEMPTS=${ATTEMPTS:-3}
mkdir -p "$L"
ERR=$(mktemp "$L/cc.XXXXXX")
export LD_LIBRARY_PATH=$R/.clangpkg/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# Push the page cache out of RAM so the kernel can hand cold pages to the
# compiler instead of OOM-killing it.
evict_cache() {
  python3 - "$R" /usr/include /usr/lib64 /usr/lib/gcc >/dev/null 2>&1 <<'EOF'
import os, sys
for root in sys.argv[1:]:
    for dp, dn, fn in os.walk(root):
        for f in fn:
            p = os.path.join(dp, f)
            try:
                if os.stat(p).st_size < (1 << 18):
                    continue
                fd = os.open(p, os.O_RDONLY)
                os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
                os.close(fd)
            except OSError:
                pass
EOF
}

target=""
src=""
prev=""
for a in "$@"; do
  case "$a" in
    *lgraph_cypher_lib.dir*) target=cypher;;
    *lgraph_server_lib.dir*) target=server;;
    *bolt.dir*) target=bolt;;
  esac
  if [ "$prev" = "-c" ]; then src="$a"; fi
  prev="$a"
done

# Which family PCH covers this translation unit.
pch_for_src() {
  case "$1" in
    */http/import_manager.cpp)     echo pch_http2 ;;
    */monitor/prometheus_monitor.cpp) echo pch_base ;;
    */restful/server/stdafx.cpp)   echo pch_base ;;
    */server/db_management_client.cpp) echo pch_light ;;
    */import/import_v2.cpp)         echo pch_import ;;
    */import/import_online.cpp)     echo pch_import ;;
    */import/import_v3.cpp)         echo pch_import ;;
    */import/blob_writer.cpp)       echo pch_import ;;
    */restful/server/rest_server.cpp) echo pch_l5b ;;
    */server/state_machine.cpp)     echo pch_l5b ;;
    */server/bolt_handler.cpp)      echo pch_l5b ;;
    */http/http_server.cpp)         echo pch_l5b ;;
    */protobuf/ha.pb.cc) echo "" ;;
    */protobuf/tugraph_db_management.pb.cc) echo "" ;;
    *) echo pch_l4 ;;
  esac
}

raw=()
skip=0
for a in "$@"; do
  if [ $skip -eq 1 ]; then skip=0; continue; fi
  case "$a" in
    *g++|*/c++) raw+=("$CLANG");;
    *gcc|*/cc)  raw+=("$R/.clangpkg/usr/bin/clang");;
    --param) skip=1;;
    --param=*|-ftrack-macro-expansion=*) ;;
    -fopenmp) raw+=("-fopenmp=libgomp");;
    *) raw+=("$a");;
  esac
done

pch=""
if [ "$target" = "cypher" ]; then
  # The op.h PCH was built with the plain GCC include dir and without
  # -fopenmp; keep that exact combination or clang rejects it.
  raw=("${raw[@]/-fopenmp=libgomp/}")
  incs=("-I$GCCDIR/include")
  [ -f "$CYPHER_PCH" ] && pch="$CYPHER_PCH"
elif [ "$target" = "bolt" ]; then
  # The bolt static library is built without -fPIC, so it cannot share the
  # server PCH chain ("__PIC__ level differs in PCH file vs. current file").
  # Its closure is small enough to compile from source.
  incs=("-I$SHIM" "-I$GCCDIR/include")
else
  incs=("-I$SHIM" "-I$GCCDIR/include")
  fam=$(pch_for_src "$src")
  if [ -n "$fam" ] && [ -f "$B/$fam.h.pch" ]; then
    pch="$B/$fam.h.pch"
  fi
fi

build_args() {
  local out=("${raw[0]}" "--gcc-install-dir=$GCCDIR" "${incs[@]}" "${raw[@]:1}")
  if [ -n "$1" ]; then out=("${out[0]}" -include-pch "$1" "${out[@]:1}"); fi
  printf '%s\n' "${out[@]}"
}

rc=137
for attempt in $(seq 1 "$ATTEMPTS"); do
  # Reclaim the page cache before every attempt: the PCH pages are mapped
  # file pages, so dropping them early leaves the kernel free to evict the
  # remainder when the AST starts growing.
  evict_cache; [ "$attempt" -gt 1 ] && sleep 10
  mapfile -t CMD < <(build_args "$pch")
  /usr/bin/time -q -f '%e\t%M\t%C' -a -o "$L/tu-metrics.tsv" \
    timeout -s KILL "$TU_TIMEOUT" "${CMD[@]}" 2>"$ERR"
  rc=$?
  if [ $rc -eq 0 ]; then
    break
  fi
  if grep -q 'precompiled header' "$ERR"; then
    # Stale or incompatible PCH: drop it and compile the closure from source.
    echo "PCH-INVALID (target=$target src=$src): $*" >> "$L/retry.log"
    pch=""
    rc=1
    break
  fi
  if [ $rc -ne 137 ] && [ $rc -ne 134 ] && [ $rc -ne 124 ]; then
    break
  fi
  echo "OOM-RETRY attempt=$attempt rc=$rc pch=$(basename "${pch:-none}") peak=$(tail -1 "$L/tu-metrics.tsv" | cut -f2) src=$src" >> "$L/retry.log"
done

cat "$ERR" >&2
rm -f "$ERR"
exit $rc
