#!/usr/bin/env bash
# Phase 0 common container configuration.
#
# Source this file; it does not execute anything on its own.
#
#   source "$(dirname "${BASH_SOURCE[0]}")/container.sh"
#
# Everything is overridable from the environment so the same scripts work on a
# developer machine, in the devcontainer, and in CI.

set -euo pipefail

PHASE0_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${PHASE0_DIR}/../.." && pwd)"

# Pinned images. Override only when deliberately testing a different environment,
# and record the new digest in ci/phase0/images.lock if you do.
PHASE0_COMPILE_IMAGE="${PHASE0_COMPILE_IMAGE:-tugraph-compile-arm64:phase0}"
PHASE0_RUNTIME_IMAGE="${PHASE0_RUNTIME_IMAGE:-tugraph-runtime-arm64:local}"
PHASE0_PLATFORM="${PHASE0_PLATFORM:-linux/arm64}"

# Workspace layout inside the container.
PHASE0_WORKDIR="${PHASE0_WORKDIR:-/workspace}"

# Named volume for benchmark scratch data. Kept OUT of the bind mount on
# purpose: on macOS the bind mount is virtiofs, whose mmap/fsync behaviour is
# not representative of a normal Linux filesystem and would distort the
# storage/backup/restore numbers. The named volume lives in the Docker VM's
# own filesystem instead.
PHASE0_DATA_VOLUME="${PHASE0_DATA_VOLUME:-tugraph-phase0-data}"

# Applies to every container that starts an lgraph_server.
PHASE0_ULIMIT_NOFILE="${PHASE0_ULIMIT_NOFILE:-1048576:1048576}"

# Space-separated KEY=VALUE pairs forwarded as `docker run -e`. Used by
# run_tests.sh / run_bench.sh to pass mode selection into the container.
# Values must not contain spaces.
#
# macOS ships bash 3.2, so this deliberately avoids namerefs (`local -n`) and
# `mapfile`, and callers expand the array as ${_PHASE0_ENV_ARGS[@]+"${_PHASE0_ENV_ARGS[@]}"}
# to stay safe under `set -u` when the array is empty.
PHASE0_EXTRA_ENV="${PHASE0_EXTRA_ENV:-}"
_PHASE0_ENV_ARGS=()

_phase0_fill_extra_env() {
    _PHASE0_ENV_ARGS=()
    local kv
    for kv in ${PHASE0_EXTRA_ENV}; do
        _PHASE0_ENV_ARGS[${#_PHASE0_ENV_ARGS[@]}]="-e"
        _PHASE0_ENV_ARGS[${#_PHASE0_ENV_ARGS[@]}]="$kv"
    done
}

phase0_die() { echo "phase0: $*" >&2; exit 1; }

phase0_require_docker() {
    command -v docker >/dev/null 2>&1 || phase0_die "docker not found in PATH"
    docker info >/dev/null 2>&1 || phase0_die "docker daemon is not reachable"
}

# Verify the pinned image is present and (optionally) that its ID matches
# images.lock. Pass --verify-id to enforce.
#
# PHASE0_SKIP_IMAGE_PIN=1 bypasses the ID comparison. This exists ONLY for CI
# running on a different architecture (e.g. amd64 GitHub runners, which cannot
# use the arm64 baseline image). Any run that sets it is NOT comparable to the
# recorded baseline, so the benchmark workflow records the actual image ID in
# the uploaded result.
phase0_check_image() {
    local image="$1" verify_id="${2:-}"
    docker image inspect "$image" >/dev/null 2>&1 \
        || phase0_die "image '$image' not found; see ci/phase0/env/README.md"
    if [[ "$verify_id" == "--verify-id" ]]; then
        if [[ "${PHASE0_SKIP_IMAGE_PIN:-0}" == "1" ]]; then
            echo "phase0: WARNING PHASE0_SKIP_IMAGE_PIN=1 - image ID not verified" >&2
            return 0
        fi
        local want id
        want="$(awk -v img="$image" '
            $0 ~ "^image" && $3 == img { found=1 }
            found && $1 == "image_id" { print $3; exit }
        ' "${PHASE0_DIR}/images.lock")"
        id="$(docker image inspect "$image" --format '{{.Id}}')"
        if [[ -n "$want" && "$want" != "$id" ]]; then
            phase0_die "image '$image' ID mismatch
  pinned:  $want
  present: $id
Update ci/phase0/images.lock only with sign-off -- changing it invalidates
every previously recorded baseline number.
(Set PHASE0_SKIP_IMAGE_PIN=1 only for CI on a different architecture.)"
        fi
    fi
}

# Run a command in the compile image with the repo bind-mounted read-write.
phase0_run_compile() {
    phase0_require_docker
    phase0_check_image "$PHASE0_COMPILE_IMAGE" --verify-id
    docker run --rm \
        --platform "$PHASE0_PLATFORM" \
        --ulimit "nofile=${PHASE0_ULIMIT_NOFILE}" \
        -v "${REPO_ROOT}:${PHASE0_WORKDIR}" \
        -w "${PHASE0_WORKDIR}" \
        -e "LD_LIBRARY_PATH=/usr/local/lib64/lgraph:/usr/local/lib64:/usr/local/lib:/usr/lib/jvm/java-11-openjdk/lib/server" \
        -e "PYTHONPATH=/usr/local/lib64/lgraph:/usr/local/lib64" \
        "$PHASE0_COMPILE_IMAGE" "$@"
}

# Run a command in the benchmark container:
#   - compile image (has the built libs and python), and
#   - the named data volume mounted at /data for scratch databases.
# Extra docker flags can be passed via PHASE0_DOCKER_EXTRA, e.g.
#   PHASE0_DOCKER_EXTRA="--cpus=4 --memory=6g" ci/phase0/run_bench.sh ...
phase0_run_bench() {
    phase0_require_docker
    phase0_check_image "$PHASE0_COMPILE_IMAGE" --verify-id
    docker volume inspect "$PHASE0_DATA_VOLUME" >/dev/null 2>&1 \
        || docker volume create "$PHASE0_DATA_VOLUME" >/dev/null
    local image_id
    image_id="$(docker image inspect "$PHASE0_COMPILE_IMAGE" --format '{{.Id}}')"
    _phase0_fill_extra_env
    # shellcheck disable=SC2086
    docker run --rm \
        --platform "$PHASE0_PLATFORM" \
        --ulimit "nofile=${PHASE0_ULIMIT_NOFILE}" \
        --init \
        -v "${REPO_ROOT}:${PHASE0_WORKDIR}" \
        -v "${PHASE0_DATA_VOLUME}:/data" \
        -w "${PHASE0_WORKDIR}" \
        -e "PHASE0_IMAGE_ID=${image_id}" \
        -e "PHASE0_REPO_ROOT=${PHASE0_WORKDIR}" \
        -e "PHASE0_WORKDIR=${PHASE0_WORKDIR}" \
        -e "LD_LIBRARY_PATH=/usr/local/lib64/lgraph:/usr/local/lib64:/usr/local/lib:/usr/lib/jvm/java-11-openjdk/lib/server" \
        -e "PYTHONPATH=/usr/local/lib64/lgraph:/usr/local/lib64" \
        -e "PHASE0_DATA_DIR=/data" \
        ${_PHASE0_ENV_ARGS[@]+"${_PHASE0_ENV_ARGS[@]}"} \
        ${PHASE0_DOCKER_EXTRA:-} \
        "$PHASE0_COMPILE_IMAGE" "$@"
}
