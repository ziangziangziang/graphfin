#!/usr/bin/env bash
# Local QA preflight: verify the pinned toolchain is complete and sane.
#
# Run this before trusting any build or benchmark result.
#   qa/local/run.sh doctor
#
# Exits non-zero if a required component is missing.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/container.sh"

qa_require_docker

echo "=== host ==="
echo "host arch:   $(uname -m)"
echo "host os:     $(uname -sr)"
echo "docker:      $(docker version --format '{{.Server.Version}}')"
echo "vm memory:   $(docker info --format '{{.MemTotal}}') bytes"
echo "vm cpus:     $(docker info --format '{{.NCPU}}')"
echo

echo "=== images ==="
qa_check_image "$QA_COMPILE_IMAGE" --verify-id
echo "compile:     $QA_COMPILE_IMAGE $(docker image inspect "$QA_COMPILE_IMAGE" --format '{{.Id}}')"
# The runtime image validates the install layout; it is not needed for builds,
# tests or benchmarks and is not built by the x86 from-source path.
if docker image inspect "$QA_RUNTIME_IMAGE" >/dev/null 2>&1; then
    qa_check_image "$QA_RUNTIME_IMAGE" --verify-id
    echo "runtime:     $QA_RUNTIME_IMAGE $(docker image inspect "$QA_RUNTIME_IMAGE" --format '{{.Id}}')"
else
    echo "runtime:     $QA_RUNTIME_IMAGE absent (optional)"
fi
echo

echo "=== container preflight ==="
qa_run_compile bash -lc '
set -uo pipefail
fail=0
check() {
    local label="$1"; shift
    if out=$("$@" 2>&1); then
        printf "  %-22s OK   %s\n" "$label" "$(echo "$out" | head -1)"
    else
        printf "  %-22s FAIL %s\n" "$label" "$(echo "$out" | head -1)"
        fail=1
    fi
}
check_file() {
    local label="$1"; local path="$2"
    if compgen -G "$path" >/dev/null; then
        printf "  %-22s OK   %s\n" "$label" "$(ls $path | head -1)"
    else
        printf "  %-22s FAIL no match: %s\n" "$label" "$path"
        fail=1
    fi
}

echo "  arch: $(uname -m)"
check gcc        gcc --version
check cmake      cmake --version
check make       make --version
check protoc     protoc --version
check python3    python3 --version
check cython     cython --version
check nproc      nproc

for spec in \
    "brpc:/usr/local/lib64/libbrpc*" \
    "braft:/usr/local/lib/libbraft*" \
    "rocksdb:/usr/lib64/librocksdb*" \
    "leveldb:/usr/local/lib/libleveldb*" \
    "cpprest:/usr/local/lib64/libcpprest*" \
    "antlr4:/usr/local/lib/libantlr4-runtime*" \
    "gtest:/usr/local/lib64/libgtest.a" \
    "gmock:/usr/local/lib64/libgmock.a" \
    "boost:/usr/local/include/boost/version.hpp" \
    "openssl:/usr/lib64/libssl.a" \
    "vsag:/usr/local/lib*/libvsag*" \
    "arrow:/usr/local/lib64/libarrow*" \
    "gflags:/usr/local/lib*/libgflags*" \
    "glog:/usr/local/lib*/libglog*" \
    "snappy:/usr/local/lib*/libsnappy*" \
    "protobuf:/usr/local/lib*/libprotobuf*" \
; do
    check_file "${spec%%:*}" "${spec#*:}"
done

echo "  --- optional features ---"
if [[ -f /usr/lib/jvm/java-11-openjdk/include/jni.h ]]; then
    echo "  jni headers:         present (fulltext index can be built)"
else
    echo "  jni headers:         absent -- ENABLE_FULLTEXT_INDEX must stay OFF"
fi
if [[ -x /usr/lib/jvm/java-11-openjdk/bin/java ]]; then
    echo "  java 11:             $(/usr/lib/jvm/java-11-openjdk/bin/java -version 2>&1 | head -1)"
fi

echo "  --- limits ---"
echo "  nofile:      $(ulimit -n)"
echo "  max_map_cnt: $(cat /proc/sys/vm/max_map_count 2>/dev/null || echo n/a)"
echo "  cgroup mem:  $(cat /sys/fs/cgroup/memory.max 2>/dev/null \
                       || cat /sys/fs/cgroup/memory/memory.limit_in_bytes 2>/dev/null \
                       || echo n/a)"
if [[ "$fail" == "1" ]]; then
    echo "PREFLIGHT FAILED: the compile image is incomplete." >&2
    exit 1
fi
echo "PREFLIGHT OK"
'
