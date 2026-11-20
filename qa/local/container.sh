#!/usr/bin/env bash
# Common container configuration for the local QA harness.
#
# Source this file; it does not execute anything on its own.
#
#   source "$(dirname "${BASH_SOURCE[0]}")/container.sh"
#
# Everything is overridable from the environment so the same scripts work on a
# developer machine, in the devcontainer, and in CI.

set -euo pipefail

QA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${QA_DIR}/../.." && pwd)"

# Pinned images. Override only when deliberately testing a different environment,
# and record the new digest in qa/local/images.lock if you do.
#
# The arm64 pin is the historical baseline environment. On x86_64 the
# from-source image built by qa/local/build-image.sh is the supported path; its
# ID is recorded in each result rather than pinned here.
case "$(uname -m)" in
    arm64|aarch64)
        _qa_default_compile=graphfin-compile-arm64:local
        _qa_default_runtime=graphfin-runtime-arm64:local
        _qa_default_platform=linux/arm64
        ;;
    *)
        _qa_default_compile=graphfin-compile-amd64:from-source
        _qa_default_runtime=graphfin-runtime-amd64:local
        _qa_default_platform=linux/amd64
        ;;
esac
QA_COMPILE_IMAGE="${QA_COMPILE_IMAGE:-${_qa_default_compile}}"
QA_RUNTIME_IMAGE="${QA_RUNTIME_IMAGE:-${_qa_default_runtime}}"
QA_PLATFORM="${QA_PLATFORM:-${_qa_default_platform}}"

# Workspace layout inside the container.
QA_WORKDIR="${QA_WORKDIR:-/workspace}"

# Named volume for benchmark scratch data. Kept OUT of the bind mount on
# purpose: on macOS the bind mount is virtiofs, whose mmap/fsync behaviour is
# not representative of a normal Linux filesystem and would distort the
# storage/backup/restore numbers. The named volume lives in the Docker VM's
# own filesystem instead.
QA_DATA_VOLUME="${QA_DATA_VOLUME:-graphfin-qa-data}"

# Applies to every container that starts an lgraph_server.
QA_ULIMIT_NOFILE="${QA_ULIMIT_NOFILE:-1048576:1048576}"

# Space-separated KEY=VALUE pairs forwarded as `docker run -e`. Used by
# run.sh to pass mode selection into the container. Values must not contain
# spaces.
#
# macOS ships bash 3.2, so this deliberately avoids namerefs (`local -n`) and
# `mapfile`, and callers expand the array as ${_QA_ENV_ARGS[@]+"${_QA_ENV_ARGS[@]}"}
# to stay safe under `set -u` when the array is empty.
QA_EXTRA_ENV="${QA_EXTRA_ENV:-}"
_QA_ENV_ARGS=()

_qa_fill_extra_env() {
    _QA_ENV_ARGS=()
    local kv
    for kv in ${QA_EXTRA_ENV}; do
        _QA_ENV_ARGS[${#_QA_ENV_ARGS[@]}]="-e"
        _QA_ENV_ARGS[${#_QA_ENV_ARGS[@]}]="$kv"
    done
}

qa_die() { echo "qa: $*" >&2; exit 1; }

qa_require_docker() {
    command -v docker >/dev/null 2>&1 || qa_die "docker not found in PATH"
    docker info >/dev/null 2>&1 || qa_die "docker daemon is not reachable"
}

# Verify the pinned image is present and (optionally) that its ID matches
# images.lock. Pass --verify-id to enforce.
#
# QA_SKIP_IMAGE_PIN=1 bypasses the ID comparison. This exists ONLY for CI
# running on a different architecture (e.g. amd64 GitHub runners, which cannot
# use the arm64 baseline image). Any run that sets it is NOT comparable to the
# recorded baseline, so the benchmark workflow records the actual image ID in
# the uploaded result.
qa_check_image() {
    local image="$1" verify_id="${2:-}"
    docker image inspect "$image" >/dev/null 2>&1 \
        || qa_die "image '$image' not found; see qa/local/env/README.md"
    if [[ "$verify_id" == "--verify-id" ]]; then
        if [[ "${QA_SKIP_IMAGE_PIN:-0}" == "1" ]]; then
            echo "qa: WARNING QA_SKIP_IMAGE_PIN=1 - image ID not verified" >&2
            return 0
        fi
        local want id
        want="$(awk -v img="$image" '
            $0 ~ "^image" && $3 == img { found=1 }
            found && $1 == "image_id" { print $3; exit }
        ' "${QA_DIR}/images.lock")"
        id="$(docker image inspect "$image" --format '{{.Id}}')"
        if [[ -n "$want" && "$want" != "$id" ]]; then
            qa_die "image '$image' ID mismatch
  pinned:  $want
  present: $id
Update qa/local/images.lock only with sign-off -- changing it invalidates
every previously recorded baseline number.
(Set QA_SKIP_IMAGE_PIN=1 only for CI on a different architecture.)"
        fi
    fi
}

# Run a command in the compile image with the repo bind-mounted read-write.
qa_run_compile() {
    qa_require_docker
    qa_check_image "$QA_COMPILE_IMAGE" --verify-id
    docker run --rm \
        --platform "$QA_PLATFORM" \
        --ulimit "nofile=${QA_ULIMIT_NOFILE}" \
        -v "${REPO_ROOT}:${QA_WORKDIR}" \
        -w "${QA_WORKDIR}" \
        -e "LD_LIBRARY_PATH=/usr/local/lib64/lgraph:/usr/local/lib64:/usr/local/lib:/usr/lib/jvm/java-11-openjdk/lib/server" \
        -e "PYTHONPATH=/usr/local/lib64/lgraph:/usr/local/lib64" \
        "$QA_COMPILE_IMAGE" "$@"
}

# Run a command in the benchmark container:
#   - compile image (has the built libs and python), and
#   - the named data volume mounted at /data for scratch databases.
# Extra docker flags can be passed via QA_DOCKER_EXTRA, e.g.
#   QA_DOCKER_EXTRA="--cpus=4 --memory=6g" qa/local/run.sh bench ...
qa_run_bench() {
    qa_require_docker
    qa_check_image "$QA_COMPILE_IMAGE" --verify-id
    docker volume inspect "$QA_DATA_VOLUME" >/dev/null 2>&1 \
        || docker volume create "$QA_DATA_VOLUME" >/dev/null
    local image_id
    image_id="$(docker image inspect "$QA_COMPILE_IMAGE" --format '{{.Id}}')"
    _qa_fill_extra_env
    # shellcheck disable=SC2086
    docker run --rm \
        --platform "$QA_PLATFORM" \
        --ulimit "nofile=${QA_ULIMIT_NOFILE}" \
        --init \
        -v "${REPO_ROOT}:${QA_WORKDIR}" \
        -v "${QA_DATA_VOLUME}:/data" \
        -w "${QA_WORKDIR}" \
        -e "QA_IMAGE_ID=${image_id}" \
        -e "QA_REPO_ROOT=${QA_WORKDIR}" \
        -e "QA_WORKDIR=${QA_WORKDIR}" \
        -e "LD_LIBRARY_PATH=/usr/local/lib64/lgraph:/usr/local/lib64:/usr/local/lib:/usr/lib/jvm/java-11-openjdk/lib/server" \
        -e "PYTHONPATH=/usr/local/lib64/lgraph:/usr/local/lib64" \
        -e "QA_DATA_DIR=/data" \
        ${_QA_ENV_ARGS[@]+"${_QA_ENV_ARGS[@]}"} \
        ${QA_DOCKER_EXTRA:-} \
        "$QA_COMPILE_IMAGE" "$@"
}
