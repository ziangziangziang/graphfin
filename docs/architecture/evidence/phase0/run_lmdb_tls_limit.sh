#!/usr/bin/env bash
# Phase 0 experiment: prove the ~1000-graph ceiling is a pthread TLS-key limit.
#
# See lmdb_tls_limit.c for the full explanation. This runs the experiment inside
# the pinned compile image so the result matches the recorded baseline.
#
# Usage:
#   docs/architecture/evidence/phase0/run_lmdb_tls_limit.sh
#
# Expected:
#   TLS    FAILED opening environment #1025: rc=11 (Resource temporarily unavailable)
#   NOTLS  opened 5000 environments without failure

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../../../../qa/local/container.sh"

qa_require_docker
qa_check_image "$QA_COMPILE_IMAGE" --verify-id

echo "=== glibc TLS key limit ==="
docker run --rm --platform "$QA_PLATFORM" --entrypoint bash "$QA_COMPILE_IMAGE" -lc \
    'getconf PTHREAD_KEYS_MAX'

qa_run_compile bash -lc "
set -euo pipefail
SRC='${QA_WORKDIR}/docs/architecture/evidence/phase0/lmdb_tls_limit.c'
LMDB='${QA_WORKDIR}/src/core/lmdb'

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
