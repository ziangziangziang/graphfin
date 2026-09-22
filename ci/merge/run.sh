#!/usr/bin/env bash
# Reuse existing binaries; keep the repo read-only and all test DBs isolated.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../phase0/container.sh"
suite="${1:-smoke}"
case "$suite" in unit|smoke|clients|ha) ;; *) echo "usage: $0 {unit|smoke|clients|ha}" >&2; exit 2 ;; esac
phase0_require_docker
phase0_check_image "$PHASE0_COMPILE_IMAGE" --verify-id
results="${MERGE_RESULTS:-/tmp/graphfin-merge-results}"
mkdir -p "$results"
results="$(cd "$results" && pwd)"
image_id="$(docker image inspect "$PHASE0_COMPILE_IMAGE" --format '{{.Id}}')"
dependency_args=()
if [[ -n "${MERGE_PYTHON_DEPS:-}" ]]; then
  dependency_args=(-v "${MERGE_PYTHON_DEPS}:/python-deps:ro" -e MERGE_PYTHON_DEPS=/python-deps)
fi
docker run --rm --init --platform "$PHASE0_PLATFORM" \
  --memory "${MERGE_MEMORY:-6g}" --cpus "${MERGE_CPUS:-4}" \
  --ulimit "nofile=${PHASE0_ULIMIT_NOFILE}" \
  -v "${REPO_ROOT}:/workspace:ro" -v "${results}:/results" \
  -e "MERGE_IMAGE_ID=${image_id}" -e "MERGE_MEMORY=${MERGE_MEMORY:-6g}" \
  -e 'LD_LIBRARY_PATH=/usr/local/lib64/lgraph:/usr/local/lib64:/usr/local/lib:/usr/lib/jvm/java-11-openjdk/lib/server' \
  ${dependency_args[@]+"${dependency_args[@]}"} \
  "$PHASE0_COMPILE_IMAGE" python3 /workspace/ci/merge/run.py \
  --suite "$suite" --out /results --timeout "${MERGE_TIMEOUT:-600}"
