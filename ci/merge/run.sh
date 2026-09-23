#!/usr/bin/env bash
# Reuse existing binaries; keep the repo read-only and all test DBs isolated.
set -euo pipefail
# Load container helpers (image name, platform, ulimits, REPO_ROOT).
source "$(dirname "${BASH_SOURCE[0]}")/../phase0/container.sh"
# Pick suite: unit | smoke | clients | ha.
suite="${1:-smoke}"
case "$suite" in unit|smoke|clients|ha) ;; *) echo "usage: $0 {unit|smoke|clients|ha}" >&2; exit 2 ;; esac
# Fail fast if Docker is missing or the pinned image ID does not match images.lock.
phase0_require_docker
phase0_check_image "$PHASE0_COMPILE_IMAGE" --verify-id
# Isolated, absolute results directory mounted into the container as /results.
results="${MERGE_RESULTS:-/tmp/graphfin-merge-results}"
mkdir -p "$results"
results="$(cd "$results" && pwd)"
image_id="$(docker image inspect "$PHASE0_COMPILE_IMAGE" --format '{{.Id}}')"
# Optional read-only mount of pre-fetched Python deps (e.g. neo4j driver).
dependency_args=()
if [[ -n "${MERGE_PYTHON_DEPS:-}" ]]; then
  dependency_args=(-v "${MERGE_PYTHON_DEPS}:/python-deps:ro" -e MERGE_PYTHON_DEPS=/python-deps)
fi
# Run the suite inside the pinned image: repo mounted read-only, results writable.
docker run --rm --init --platform "$PHASE0_PLATFORM" \
  --memory "${MERGE_MEMORY:-6g}" --cpus "${MERGE_CPUS:-4}" \
  --ulimit "nofile=${PHASE0_ULIMIT_NOFILE}" \
  -v "${REPO_ROOT}:/workspace:ro" -v "${results}:/results" \
  -e "MERGE_IMAGE_ID=${image_id}" -e "MERGE_MEMORY=${MERGE_MEMORY:-6g}" \
  -e 'LD_LIBRARY_PATH=/usr/local/lib64/lgraph:/usr/local/lib64:/usr/local/lib:/usr/lib/jvm/java-11-openjdk/lib/server' \
  ${dependency_args[@]+"${dependency_args[@]}"} \
  "$PHASE0_COMPILE_IMAGE" python3 /workspace/ci/merge/run.py \
  --suite "$suite" --out /results --timeout "${MERGE_TIMEOUT:-600}"
