# GraphFin CI

Last reviewed: September 30, 2026.

This document describes the workflows in this checkout, including the separate website checks, the `site/**` trigger policy, and removal of the three issue bots. At the review date, GitHub `main` was at `e5af95bb`; the PR engine gate, website workflow split, and issue-bot removal described here had not reached `main`. The local configuration becomes the default configuration after it is merged. Workflow files and repository settings remain the source of truth.

## Why we run CI

GraphFin combines graph records, time-series storage, graph lifecycle management, and replication. Changes can affect stored data, tenant isolation, recovery, or client behavior even when they compile successfully. CI provides repeatable checks and records failures so maintainers can assess a change before and after merging it.

We separate checks by purpose and cost:

- Fast checks catch coding and documentation mistakes early.
- Targeted engine tests exercise important database behavior on relevant pull requests.
- Full unit, sanitizer, and integration suites exercise a wider surface on `main`.
- Website checks run when `site/` changes, and deployment validates the built website before publishing it.
- CodeQL examines code and workflow security alongside the runtime tests.

A passing job is evidence for the checks that actually ran. It is not, by itself, proof of release readiness, complete HA qualification, or production safety. The remaining qualification scope is recorded in the [post-merge acceptance plan](docs/testing/post-merge.md) and [release status](RELEASE.md).

## Current workflow inventory

In this table, a PR means a pull request targeting `main`. Three checked-in workflow files define eight jobs. CodeQL and Pages publishing are managed separately by GitHub.

| Workflow / job | Configuration | When it runs | What it does and why |
| --- | --- | --- | --- |
| `ci / changes` | [ci.yml](.github/workflows/ci.yml) | PRs | Detects build-relevant changes and decides whether to run the expensive PR engine gate. A diff failure requests the gate instead of treating the PR as unrelated. |
| `ci / lint` | [ci.yml](.github/workflows/ci.yml) | PRs and every push to `main` | Runs C++ lint, product/documentation checks, and the engine change-filter regression check. Provides inexpensive feedback and checks the routing logic itself. |
| `ci / pr_engine_gate` | [ci.yml](.github/workflows/ci.yml) | PRs selected by `changes` | Builds targeted C++ binaries, checks the server alias, runs selected unit and integration tests, and uploads result XML. Checks core database behavior before merge. |
| `ci / ut_no_asan` | [ci.yml](.github/workflows/ci.yml) | Every push to `main` | Builds with coverage instrumentation, runs the full unit suite, and generates coverage after successful tests. Looks for regressions beyond the PR selection. |
| `ci / ut_asan` | [ci.yml](.github/workflows/ci.yml) | Every push to `main` | Builds with AddressSanitizer and runs the full unit suite. Detects memory errors in addition to functional failures. |
| `ci / it` | [ci.yml](.github/workflows/ci.yml) | Every push to `main` | Builds the engine and C++ client test, runs the broad integration suite, and generates coverage after successful tests. Exercises interactions between components. |
| `Website checks / site` | [site.yml](.github/workflows/site.yml) | PRs and pushes to `main` with changes under `site/` | Installs locked npm dependencies, checks TypeScript, and runs website unit tests. Avoids running website-specific checks for unrelated changes. |
| `GraphFin website / build-and-deploy` | [documentation.yml](.github/workflows/documentation.yml) | Pushes to `main` with changes under `site/`; manual dispatch on `main` | Runs unit tests, creates the production build, runs Chromium browser tests, and publishes `site/dist` to `gh-pages`. Validates the website before publication. |

GitHub also supplies these workflows through repository features rather than checked-in workflow YAML:

| Workflow | Observed triggers / configuration | Purpose |
| --- | --- | --- |
| [CodeQL](https://github.com/ziangziangziang/graphfin/actions/workflows/github-code-scanning/codeql) | GitHub default setup; PR, main-push, and scheduled runs observed | Scans C/C++, Actions, Java/Kotlin, Python, and JavaScript/TypeScript. Detects security and code-analysis findings; complements runtime tests. |
| [Pages build and deployment](https://github.com/ziangziangziang/graphfin/actions/workflows/pages/pages-build-deployment) | GitHub Pages publishing from `gh-pages` | Serves the content written by `GraphFin website`. This is a downstream publishing step, not a second website test suite. |

## Change-selection policy

### Website

Both website YAML workflows use `paths: ["site/**"]` for their automatic triggers. Files anywhere under `site/`, including its tests, configuration, dependency lockfile, and README, qualify. Changes only to engine code, root documentation, root `assets/`, or workflow YAML do not match that filter.

For PRs, GitHub evaluates the PR's changed files, not just the latest commit. A PR that already changes `site/` can therefore run website checks again when a later commit changes only engine code. Push triggers evaluate the changes in the push.

The site currently references images in root `assets/`. An assets-only update requires a manual run of **GraphFin website** on `main` to rebuild and publish those images. Manual dispatch is the explicit exception to automatic path filtering.

The `Website checks` job runs TypeScript and unit checks. Production builds and browser tests currently run in the deployment workflow after merge. Repository-wide lint and CodeQL have their own triggers; the website filter does not disable them.

GitHub documents the comparison rules and large-diff limits in [workflow path filtering](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax#onpushpull_requestpull_request_targetpathspaths-ignore). A path-filtered workflow must not be made an unconditional required check: GitHub can leave its check pending when the workflow does not run. Use an always-running summary check if conditional jobs need to enforce merge policy.

### Engine

The PR filter currently matches `src/`, `test/`, `deps/`, `cmake/`, `include/`, `ci/`, `.github/workflows/ci.yml`, and paths ending in `CMakeLists.txt` or `.cmake`. It compares the PR base and head trees. [test/check_pr_filter.py](test/check_pr_filter.py) checks representative matches, non-matches, and the fallback when the diff cannot be computed.

This filter only controls the PR engine gate. The three full engine jobs still run on **every push to `main`**, including documentation-only and website-only pushes. There is currently no nightly engine workflow or main-branch engine path filter.

## Test coverage and evidence

The PR engine build uses `RelWithDebInfo`, disables procedures, and builds `unit_test`, `fma_unit_test`, `lgraph_server`, and the Python client library. Its current job timeout is 45 minutes and remains provisional until measured on GitHub.

[ci/github_pr_test.sh](ci/github_pr_test.sh) checks the generated server alias, runs the FMA suite and the `pr-unit` selection from [test/suites.json](test/suites.json), then runs nine integration files covering series smoke behavior, multiple graphs, access control, restart, backup/restore, graph limits, and eviction. The PR selection does not include the separate full client-contract or three-node series HA qualification suites.

[test/check_results.py](test/check_results.py) validates the PR unit XML: required cases, filter patterns, suite families, and binaries must be present, and failed or skipped cases are rejected. The PR integration step produces JUnit XML but does not currently apply that same strict skip/count validation. The workflow attempts to upload `testresult/` as `pr-gtest-results` even after failures; missing results make the upload fail.

The main engine jobs still compile and test inside a Docker build through [ci/github_ci.sh](ci/github_ci.sh). They do not explicitly export standalone test XML and server logs as workflow artifacts. Coverage generation follows successful tests; upload to Codecov is optional when `CODECOV_TOKEN` is configured, and an upload failure is non-blocking.

The stricter `unit`, `smoke`, `clients`, and `ha` qualification commands exist in [ci/merge/run.sh](ci/merge/run.sh) and [ci/merge/run.py](ci/merge/run.py). They reuse existing binaries, isolate test data, and record logs, XML, and source/binary manifests. No current workflow invokes this four-suite qualification runner. Its existence should not be interpreted as automatic GitHub qualification on every merge.

## Cost, permissions, and merge enforcement

CI, website checks, and website deployment cancel superseded runs on the same ref. Engine jobs use separate Docker cache scopes; website jobs cache npm downloads and use `npm ci` with the lockfile. Engine compilation is limited to two parallel build tasks to fit runner memory.

CI and website checks default to a read-only repository token. Engine jobs request `actions: write`; website publishing requests `contents: write` to update `gh-pages`. The remaining checked-in workflows do not request issue or PR write permissions.

At the review date, the [main branch rules](https://github.com/ziangziangziang/graphfin/rules/23859471) required a PR review and CodeQL scanning, but did **not** require CI status checks. A job named `pr_engine_gate` does not enforce merge policy until the repository rules require an appropriate successful check.

## Known gaps and proposed follow-up

These are audit findings and recommendations, not changes already applied by this document:

| Finding | Follow-up |
| --- | --- |
| The [September 23 main CI run](https://github.com/ziangziangziang/graphfin/actions/runs/35886669650) failed all three engine jobs. Both unit jobs aborted after test failures. | Investigate the assertions and crashes; preserve failure evidence rather than broadly suppressing tests. |
| The same integration run could not open `./test/suites.py` from `build/output`; command substitution hid that failure, so the intended procedure exclusions were not applied. | Fix the catalog path and make selection failures stop the runner before executing an incompatible suite. |
| Main jobs rebuild separately and retain limited test evidence. | Build once per source/configuration, reuse binaries, run tests outside image construction, and export reports and server logs. |
| Conditional PR checks are not required by branch rules. | Validate the PR engine gate on GitHub, then enforce an always-running summary check that rejects failures and unexpected skips. |
| Website production/browser validation happens after merge. | Consider adding it to site-changing PRs while keeping the `site/**` trigger policy. |

## Retired automation

`Issue Auto Close`, `Issue Labelled`, and `Issue Pre Close` have been removed from this checkout. Issue triage and closure are maintainer decisions; canned label replies and requests for stars add noise without checking product quality. The auto-close bot also had repeated access failures and a closure deadline inconsistent with the label bot's message.

Once this removal reaches `main`, these workflows will no longer automatically close issues or post comments. Existing issues, labels, comments, and workflow run history are preserved.

The old `phase0-baseline`, `series-gate`, `merge-qualification`, and Alipay scanner workflow files have already been removed from `main`. Some historical workflow registrations remain visible in GitHub. Preserve useful test cases and run history when retiring old branch definitions; do not count those registrations as current checks.

When changing CI, update this inventory together with the workflow files. Verify triggers and required-check settings as well as test commands, so a skipped job or an unexecuted suite is not mistaken for passing evidence.
