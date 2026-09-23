#!/usr/bin/env bash
# Phase 0 / R0: A/B the MDB_NOTLS storage policy against the unit test suite.
#
# Runs the upstream unit tests repeatedly with MDB_NOTLS forced on or off, and
# reports a crash-rate table. Exists because the first observed failure was an
# INTERMITTENT SIGSEGV, so a single run cannot attribute it to the policy.
#
# The switch is provided by test/main.cpp (`--lmdb_notls true|false`), which
# sets LMDBKvStore::use_notls_ before any test runs. That keeps the comparison
# to one binary, so nothing but the policy differs between the two arms.
#
# Usage:
#   dev/phase0/experiments/run_notls_matrix.sh on  4
#   dev/phase0/experiments/run_notls_matrix.sh off 4
#
# Each arm takes roughly 16 minutes per run. If a run crashes, the core is
# preserved and a backtrace is printed, which is the evidence needed to tell an
# LMDB reader-slot fault from an unrelated one.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../container.sh"

MODE="${1:-}"
RUNS="${2:-3}"

# fma_common's bool parser accepts only true/false. Passing "on"/"off" makes the
# binary abort at startup (SIGABRT, 0s), which looks like a crash but is just a
# bad flag value -- so map the friendly names.
case "$MODE" in
    on)  MODE=true ;;
    off) MODE=false ;;
    true|false) ;;
    *) echo "usage: $0 <on|off> [runs]" >&2; exit 2 ;;
esac

# NOTE: for a fully clean control the spawned lgraph_server subprocesses must
# also run with the policy off. Those read their own config, so additionally set
# "lmdb_notls": false in src/server/lgraph_standalone.json for the `off` arm;
# otherwise server-spawning tests (e.g. TestBackupRestore) still use the default.

phase0_require_docker
phase0_check_image "$PHASE0_COMPILE_IMAGE" --verify-id

echo "=== NOTLS $MODE x $RUNS runs ==="

phase0_run_bench bash -lc "
set -uo pipefail
cd '${PHASE0_WORKDIR}/build/output'
ln -sfn ../../test/integration/data data 2>/dev/null
echo \"core_pattern: \$(cat /proc/sys/kernel/core_pattern 2>/dev/null)\"

crashes=0
for i in \$(seq 1 ${RUNS}); do
    rm -rf testdb* .import_tmp 2>/dev/null
    t0=\$(date +%s)
    ./unit_test --lmdb_notls '${MODE}' > /tmp/ut_${MODE}_\$i.log 2>&1
    rc=\$?
    dur=\$(( \$(date +%s) - t0 ))
    last=\$(grep -a '^\[ RUN' /tmp/ut_${MODE}_\$i.log | tail -1 | sed 's/.*\] //')
    echo \"RESULT mode=${MODE} run=\$i exit=\$rc dur=\${dur}s last=\${last:-none}\"

    if [ \$rc -ge 128 ]; then
        crashes=\$(( crashes + 1 ))
        core=\$(ls -t core core.* 2>/dev/null | head -1)
        if [ -n \"\$core\" ]; then
            echo \"--- backtrace from \$core (mode=${MODE} run=\$i) ---\"
            gdb -batch -ex 'bt 30' -ex 'thread apply all bt 8' ./unit_test \"\$core\" 2>&1 | tail -45
            mv \"\$core\" /tmp/core_${MODE}_\$i
        else
            echo \"--- no core file found ---\"
        fi
    fi
    rm -rf testdb* .import_tmp 2>/dev/null
done
echo \"SUMMARY mode=${MODE} runs=${RUNS} crashes=\${crashes}\"
"
