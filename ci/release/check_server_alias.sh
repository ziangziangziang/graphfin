#!/usr/bin/env bash
# Smoke check: the GraphFin product name and the legacy compatibility name
# must both resolve to a working server binary.
#
# Usage:
#   ci/release/check_server_alias.sh [bin_dir]
#
# bin_dir defaults to build/output (build tree) and also accepts an install
# prefix's bin directory. graphfin_server is created by install(CODE ...) in
# src/CMakeLists.txt, so it only exists in an install tree, not in build/output.
#
# The probe is --version: GlobalConfig::PrintVersion returns 0 during argument
# parsing, before main reads /usr/local/etc/lgraph.json. --help cannot be used
# here because it is parsed only after that read, which exits non-zero when no
# config file is installed.
set -euo pipefail

BIN_DIR="${1:-build/output}"

fail=0
for name in lgraph_server graphfin_server; do
    bin="${BIN_DIR}/${name}"
    if [[ ! -e "$bin" ]]; then
        echo "server-alias: FAIL ${name} not found: ${bin}" >&2
        fail=1
        continue
    fi
    if [[ ! -x "$bin" ]]; then
        echo "server-alias: FAIL ${name} not executable: ${bin}" >&2
        fail=1
        continue
    fi
    rc=0
    timeout 60 "$bin" --version >/dev/null 2>&1 || rc=$?
    if [[ "$rc" == "0" ]]; then
        echo "server-alias: OK ${name} --version"
    else
        echo "server-alias: FAIL ${name} --version (rc=${rc})" >&2
        fail=1
    fi
done

# The product name is an alias, not a second binary: it must resolve to the
# compatibility name so the two cannot drift apart.
product="${BIN_DIR}/graphfin_server"
legacy="${BIN_DIR}/lgraph_server"
if [[ -L "$product" && -e "$product" ]]; then
    target="$(readlink "$product")"
    if [[ "$(cd "$(dirname "$product")" && cd "$(dirname "$target")" && pwd)/$(basename "$target")" \
          == "$(cd "$(dirname "$legacy")" && pwd)/$(basename "$legacy")" ]]; then
        echo "server-alias: OK graphfin_server -> ${target}"
    else
        echo "server-alias: FAIL graphfin_server points at ${target}, not lgraph_server" >&2
        fail=1
    fi
elif [[ -e "$product" ]]; then
    echo "server-alias: FAIL graphfin_server is not a symlink to lgraph_server" >&2
    fail=1
fi

if [[ "$fail" != "0" ]]; then
    echo "server-alias: REJECT" >&2
    exit 1
fi
echo "server-alias: ACCEPT (lgraph_server + graphfin_server)"
