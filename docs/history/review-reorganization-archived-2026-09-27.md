# GraphFin reorganization review

**Approval recommendation for PR #1:** Require a small, working code change in each of the three areas below. Keep the changes in separate commits so reviewers can verify the move, rename, and test policy independently. Advice alone does not address the PR. Preserve the existing binary, library, Python import, image, and storage interfaces while introducing GraphFin-facing names.

The minimum reviewable diff is: (1) move the local Phase 0 harness out of `ci/` and update its callers; (2) add a GraphFin build/command name with a working legacy alias; (3) put suite membership and feature-disabled exclusions in one machine-readable catalog and use it from the PR, merge, and Phase 0 runners. Each change needs the checks described below before approval. Broad deletion of tests and a wholesale `lgraph` rename are separate release decisions.

## 1. Target layout and ownership

```text
test/
  unit/                 C++ gtest sources (including current test_fma/)
  integration/          pytest sources and their fixtures
  stress/               sanitizer and long-running correctness tests
  suites.yaml           canonical suite membership and build requirements
  run.py                local suite runner and selection validation
ci/
  github_pr.sh          thin GitHub entry point
  github_ci.sh          thin main-branch entry point
  merge/                isolated post-merge execution and evidence capture
  images/               Dockerfiles and image definitions
  release/              packaging and documentation checks
dev/
  phase0/               local build, doctor, container helpers, pinned-image lock
benchmark/
  scaling/              workloads, measurements, and former run_bench scripts
  experiments/          exploratory LMDB and resource-limit probes
```

Keep `learn/`, `procedures/`, `demo/`, `toolkits/`, `site/`, and `docs/architecture/` as distinct product, example, and documentation roots. Their size alone does not make them test or CI content. Move a file only when its owner and invocation are clear. `ci/` should retain CI adapters, images, release checks, and the merge gate; reducing it to `github_*.sh` plus images would hide real CI responsibilities elsewhere.

Add `test/suites.yaml` (or JSON if avoiding a YAML dependency) and make `ci/github_pr.sh`, `ci/github_ci.sh`, the moved Phase 0 runners, and `ci/merge/run.py` consume named selections from it **in this PR**. Today the PR and merge runners duplicate a gtest filter, while the Phase 0 series runner has a broader, separate filter. Record each suite's exact gtest patterns or pytest node IDs, tier (`pr`, `merge`, `main`, `nightly`), required CMake features, required artifacts, expected minimum case count, and owner. The runner should fail if a required pattern matches zero tests, a required case is skipped, or a feature is disabled. Preserve the existing image ID checks, worktree/binary hashes, fresh XML, and isolated merge scratch directory as execution evidence; suite selection can be shared without weakening those checks.

Move `ci/phase0/` to `dev/phase0/` in this PR, update executable references (especially `ci/merge/run.sh`, container paths, benchmark drivers, and getting-started commands), and verify the image lock still resolves from the new location. Keep dated benchmark results as historical records. Check the shell syntax, local `--help`/doctor path, and at least one pinned-container suite through the moved entry point. Do not move the 130 C++ files merely to achieve a tidy tree while `test/CMakeLists.txt` still lists them explicitly.

## 2. Rename plan and compatibility boundary

Treat `GraphFin` as the product name now; treat existing `lgraph_*`, `liblgraph.so`, `TuGraphClient`, Docker tags, and installed paths as compatibility names until each has a migration contract. `PRODUCT.json` already identifies GraphFin and TuGraph 4.5.2 as the upstream compatibility baseline. Keep accurate upstream attribution in the README and dependency provenance; update product-facing descriptions without rewriting historical references or third-party URLs. **For this PR**, change the CMake project identity to GraphFin and expose a `graphfin_server` command while keeping `lgraph_server` functional; include an install/build-output smoke check for both names. This gives the PR a real, low-risk rename without changing ABI or stored data.

| Stage | Change | Required check |
| --- | --- | --- |
| 0. Inventory | Map every externally consumed binary, library SONAME, Python distribution/import, package path, image tag/digest, config path, persisted DB/backup format, and script reference. Record which are published or used by deployments. | Consumer matrix and current artifact list checked into the repo. |
| 1. Product surface (this PR) | Change the root/subproject CMake identity and add a `graphfin_server` alias or wrapper for the existing server. Align help text and release metadata where that name is surfaced. Keep explicit “derived from TuGraph 4.5.2” attribution. | Configure/build succeeds; both `graphfin_server` and `lgraph_server` start from build and install output. |
| 2. Images | Build GraphFin-tagged images from a verified Dockerfile; keep pinned Phase 0 `tugraph-*` references and image IDs intact until a new lockfile is validated. | Image digest and entry point tests pass in clean containers. |
| 3. Client and library | Add a new Python distribution/import with `TuGraphClient` compatibility imports; version and test both. Change `liblgraph.so` only after checking SONAME, dynamic linking, plugin/JNI/Cython dependencies, CMake install rules, and client ABI. Provide a real compatible library/SONAME alias if the ABI is unchanged. | Install/import tests, `ldd`/`readelf` or platform equivalent, C++/Python client and plugin tests. |
| 4. Storage and config | Rename persisted database directories, backup contents, wire identifiers, environment variables, or config keys only through explicit versioned migration or dual-read support. | Open an old DB and restore an old backup with the new release; rollback behavior documented. |

Do not blanket-replace `lgraph` in internal C++ identifiers or serialized formats. `src/CMakeLists.txt` installs into `lib64/lgraph` and `share/lgraph`; `ci/merge/run.py` names `lgraph_server`, `liblgraph.so`, `liblgraph_client_python.so`, and the `TuGraphClient` source path; `ci/phase0/images.lock` pins old image tags to immutable IDs. Those are concrete rename dependencies, not cosmetic leftovers. Define the supported old-name window and removal criteria before any public rename PR.

## 3. Legacy-suite triage rule

Classify **test cases, not whole directories**, in a tracked inventory with owner, supported behavior, required build flags, last clean result, failure issue, and next review date. In this PR, encode the current `BUILD_PROCEDURE=OFF` exclusions in the suite catalog and have both main and Phase 0 runners apply them without deleting staged test files. Keep known reds visible in test reports; do not add a broad exclusion until its case IDs, owner, and issue are recorded.

| Class | Rule and gate |
| --- | --- |
| Required | Tests current GraphFin contracts, storage integrity, schema/transactions, multi-graph ceilings and lifecycle, series behavior, backup/restore, and relevant client/HA paths. Must run in a compatible required gate, fail on zero selection or skips. Keep `TestSeries*`, schema/transaction coverage, `test_multi_graph_*`, ceiling/lifecycle regressions, and `test_merge_series*` here after validating exact case discovery. |
| Supported feature | Still promised functionality, but needs `BUILD_PROCEDURE=ON`, fulltext/JNI, OLAP, or another optional build. Run in a separate feature-enabled main or scheduled job; a build with the feature OFF cannot count as a pass or justify deletion. |
| Quarantine | Reproducible known failure with an issue, owner, failing case IDs, observed and expected behavior, and expiry/review date. Run separately and report red; never silently drop a previously required case from a filter. A newly failing GraphFin regression remains blocking. |
| Retire | Feature has been explicitly removed from the supported product, test duplicates another maintained contract with equal coverage, or it tests an obsolete upstream implementation detail. Delete only after recording the replacement or removal decision and checking that no unique compatibility contract is lost. Slowness or a disabled feature flag alone is insufficient. |

For stale goldens, first decide whether the changed output is an intentional GraphFin contract. Compare normalized semantic results: schema, rows and ordering where promised, numeric values with a justified tolerance, and error category. Update a golden only with a linked behavior change and a focused rerun. Do not use a broad tolerance to mask numerical errors, or update expected text solely to turn CI green. `TestCypherV2.TestProcedure` and procedure/LMDB expectations should be split into precise cases: repair a real supported contract, run feature-dependent cases in the proper build, or quarantine a documented legacy mismatch. Review every waiver at least once per release; expired waivers fail the gate.

Finally, preserve a small required C++ correctness slice on PRs for engine changes. The current `.github/workflows/ci.yml` runs lint/site on PRs and places C++ jobs only on `push:main`; a 70-minute post-merge suite detects regressions after merge. Use the shared catalog for a targeted PR build/test job (or an equivalent pre-merge required merge gate) with hard case-count and no-skip checks. Keep broader ASan, integration, and optional-feature matrices on main or scheduled runs.
