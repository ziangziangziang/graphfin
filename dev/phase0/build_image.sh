#!/usr/bin/env bash
# Build the Phase 0 compile image FROM THIS CHECKOUT (x86_64/amd64).
#
# The arm64 image pinned in dev/phase0/images.lock cannot be rebuilt from HEAD:
# its Dockerfile revision COPYs files (ci/images/vendor/...) that are absent from
# this tree (see dev/phase0/env/README.md "Provenance gap"). This script provides
# a reproducible-from-source alternative on the x86_64 path used by CI and the
# test host:
#
#   dev/phase0/build_image.sh
#
# It builds ci/images/tugraph-compile-centos7-Dockerfile (self-contained: no
# external COPY/ADD) and prints the resulting image id, so a run can be anchored
# to an image that is reproducible from this commit:
#
#   PHASE0_COMPILE_IMAGE=tugraph-compile-amd64:from-source dev/phase0/build.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${PHASE0_COMPILE_IMAGE_AMD64:-tugraph-compile-amd64:from-source}"
DOCKERFILE="${PHASE0_DOCKERFILE:-${REPO_ROOT}/ci/images/tugraph-compile-centos7-Dockerfile}"

echo "phase0: building ${IMAGE} from ${DOCKERFILE}"
docker build -f "${DOCKERFILE}" -t "${IMAGE}" "${REPO_ROOT}"

image_id="$(docker image inspect "${IMAGE}" --format '{{.Id}}')"
commit="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "phase0: image_id=${image_id} source_commit=${commit}"
echo "phase0: use PHASE0_COMPILE_IMAGE=${IMAGE} for reproducible runs;"
echo "phase0: record the image_id above with any result produced from it."
