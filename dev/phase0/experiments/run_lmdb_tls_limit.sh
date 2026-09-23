#!/usr/bin/env bash
# Phase 0 experiment: prove the ~1000-graph ceiling is a pthread TLS-key limit.
#
# See lmdb_tls_limit.c for the full explanation. This runs the experiment inside
# the pinned compile image so the result matches the recorded baseline.
#
# Usage:
#   dev/phase0/experiments/run_lmdb_tls_limit.sh
#
# Expected:
#   TLS    FAILED opening environment #1025: rc=11 (Resource temporarily unavailable)
#   NOTLS  opened 5000 environments without failure

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../container.sh"

phase0_require_docker
phase0_check_image "$PHASE0_COMPILE_IMAGE" --verify-id

echo "=== glibc TLS key limit ==="
docker run --rm --platform "$PHASE0_PLATFORM" --entrypoint bash "$PHASE0_COMPILE_IMAGE" -lc \
    'getconf PTHREAD_KEYS_MAX'

phase0_run_compile bash -lc "
set -euo pipefail
SRC='${PHASE0_WORKDIR}/dev/phase0/experiments/lmdb_tls_limit.c'
LMDB='${PHASE0_WORKDIR}/src/core/lmdb'

gcc -O0 -I\"\$LMDB\" -o /tmp/lmdb_tls_limit \"\$SRC\" \"\$LMDB/mdb.c\" \"\$LMDB/midl.c\" -lpthread

echo
echo '=== default flags (exactly what TuGraph uses: NO MDB_NOTLS) ==='
rm -rf /tmp/lmdb_tls_envs; mkdir -p /tmp/lmdb_tls_envs
/tmp/lmdb_tls_limit tls || true

echo
echo '=== with MDB_NOTLS added ==='
rm -rf /tmp/lmdb_tls_envs; mkdir -p /tmp/lmdb_tls_envs
/tmp/lmdb_tls_limit notls || true

rm -rf /tmp/lmdb_tls_envs
"
