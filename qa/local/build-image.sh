#!/usr/bin/env bash
# Build the local compile image FROM THIS CHECKOUT (x86_64/amd64).
#
# The arm64 image pinned in qa/local/images.lock cannot be rebuilt from HEAD:
# its Dockerfile revision COPYs files (ci/images/vendor/...) that are absent from
# this tree (see qa/local/env/README.md "Provenance gap"). This script provides
# a reproducible-from-source alternative on the x86_64 path used by CI and the
# test host:
#
#   qa/local/run.sh build image
#
# It builds ci/images/tugraph-compile-centos7-Dockerfile (self-contained: no
# external COPY/ADD) and prints the resulting image id, so a run can be anchored
# to an image that is reproducible from this commit:
#
#   QA_COMPILE_IMAGE=graphfin-compile-amd64:from-source qa/local/run.sh build
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${QA_COMPILE_IMAGE_AMD64:-graphfin-compile-amd64:from-source}"
DOCKERFILE="${QA_DOCKERFILE:-${REPO_ROOT}/ci/images/tugraph-compile-centos7-Dockerfile}"

echo "qa: building ${IMAGE} from ${DOCKERFILE}"
docker build -f "${DOCKERFILE}" -t "${IMAGE}" "${REPO_ROOT}"

image_id="$(docker image inspect "${IMAGE}" --format '{{.Id}}')"
commit="$(git -C "${REPO_ROOT}" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "qa: image_id=${image_id} source_commit=${commit}"
echo "qa: use QA_COMPILE_IMAGE=${IMAGE} for reproducible runs;"
echo "qa: record the image_id above with any result produced from it."
