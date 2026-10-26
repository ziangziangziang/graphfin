#!/bin/bash
# Single source of truth for the compiler command line.
#
# The CMake build invokes g++ with a mix of GCC-only and portable flags.
# We hand the job to clang 17 instead, which needs three adjustments:
#   * -fopenmp            -> -fopenmp=libgomp (clang has no libomp here, but it
#                           can drive GCC 10's libgomp)
#   * --param ggc-*       -> dropped (GCC-only; clang rejects them)
#   * <xmmintrin.h> etc.  -> resolved through .clangpkg/include-shim, because
#                           GCC's internal include dir shadows clang's own
#                           intrinsics headers and boost/uuid pulls them in.
#
# The PCH chain has to be compiled with *exactly* the same flags as the
# translation units that use it, otherwise clang rejects it ("OpenMP support
# and version of OpenMP differs in PCH file vs. current file").
R=/home/pdfexe/graphfin
B=$R/build
CLANGDIR=$R/.clangpkg
SHIM=$CLANGDIR/include-shim
GCCDIR=/usr/lib/gcc/x86_64-redhat-linux/10
export LD_LIBRARY_PATH=$CLANGDIR/usr/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

# Pull CXX_DEFINES / CXX_INCLUDES / CXX_FLAGS straight out of CMake's
# flags.make so the PCH chain can never drift from the real build.
FLAGS_MAKE=${FLAGS_MAKE:-$B/src/CMakeFiles/lgraph_server_lib.dir/flags.make}
_cmake_flags() { grep "^$1" "$FLAGS_MAKE" | sed "s/^$1 = //"; }

CXX_DEFINES=$(_cmake_flags CXX_DEFINES)
CXX_INCLUDES=$(_cmake_flags CXX_INCLUDES)
CXX_FLAGS=$(_cmake_flags CXX_FLAGS)

# Normalise the flag list for clang.
_norm_flags() {
  local a skip=0
  for a in "$@"; do
    if [ $skip -eq 1 ]; then skip=0; continue; fi
    case "$a" in
      --param) skip=1 ;;
      --param=*|-ftrack-macro-expansion=*) ;;
      -fopenmp) printf '%s\n' "-fopenmp=libgomp" ;;
      *) printf '%s\n' "$a" ;;
    esac
  done
}

# Flags for compiling a translation unit (no PCH).
TU_FLAGS=()
while IFS= read -r line; do TU_FLAGS+=("$line"); done < <(_norm_flags $CXX_DEFINES $CXX_INCLUDES $CXX_FLAGS)
TU_FLAGS+=("-w")

# Extra flags for *creating* a PCH: same language flags, but the OpenMP
# version has to match and warnings stay off.
PCH_FLAGS=()
while IFS= read -r line; do PCH_FLAGS+=("$line"); done < <(_norm_flags $CXX_DEFINES $CXX_INCLUDES $CXX_FLAGS)
PCH_FLAGS+=("-w" "-fno-omit-frame-pointer")

CLANG=$CLANGDIR/usr/bin/clang++
CLANG_COMMON=("--gcc-install-dir=$GCCDIR" "-I$SHIM" "-I$GCCDIR/include")

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
