#!/usr/bin/env bash
# Smoke check: the GraphFin product name and the legacy compatibility name
# must both resolve to a working server binary.
#
# Usage:
#   ci/release/check_server_alias.sh [bin_dir]
#
# bin_dir defaults to build/output (build tree) and also accepts an install
# prefix's bin directory. Each name must exist, be executable, and respond to
# --help (which prints usage and exits without starting the server).
set -euo pipefail

BIN_DIR="${1:-build/output}"

fail=0
for name in lgraph_server graphfin_server; do
    bin="${BIN_DIR}/${name}"
    if [[ ! -x "$bin" ]]; then
        echo "server-alias: FAIL ${name} missing or not executable: ${bin}" >&2
        fail=1
        continue
    fi
    if timeout 60 "$bin" --help >/dev/null 2>&1; then
        echo "server-alias: OK ${name} --help"
    else
        echo "server-alias: FAIL ${name} --help (rc=$?)" >&2
        fail=1
    fi
done

if [[ "$fail" != "0" ]]; then
    echo "server-alias: REJECT" >&2
    exit 1
fi
echo "server-alias: ACCEPT (lgraph_server + graphfin_server)"
