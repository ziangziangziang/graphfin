#!/bin/bash
# Fast PR path: lint + targeted build/tests only.
# Full suite (asan, coverage, sphinx docs, web UI, full ut/it) remains in
# ci/github_ci.sh (driven by BUILD_TYPE/TEST_TYPE from the main-push jobs).
#
# Runs both PR phases back to back for local/dev use. CI runs them in
# separate steps (build inside the image, tests via `docker run` with a
# mounted results directory) so test evidence is exportable even on failure.
set -euo pipefail

bash "$(dirname "${BASH_SOURCE[0]}")/github_pr_build.sh"
bash "$(dirname "${BASH_SOURCE[0]}")/github_pr_test.sh"
