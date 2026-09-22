# GraphFin v0.1.0-alpha — Delegated Work Plan

This task coordinates the remaining work after merging the performance and
time-series branches. GraphFin remains a general-purpose graph database with
native time-series data; the financial dependency-analysis use case is the
primary acceptance fixture, together with a non-financial telemetry fixture.

The release identity is confirmed:

- Product: `GraphFin`
- Product version: `0.1.0-alpha`
- Git tag: `v0.1.0-alpha`
- Engine compatibility baseline: TuGraph `4.5.2`
- Repository: `https://github.com/ziangziangziang/graphfin`

This plan is for delegation. Each delegated agent must report the exact commit,
commands, environment, artifacts, and pass/fail result. Agents must not claim
release readiness from source inspection alone.

## Current gate status

The merged qualification currently has this evidence:

| Gate | Current result | Decision |
| --- | --- | --- |
| C++ merged unit suite | 110/110 passed, zero skips, clean tree at `5e31bacf` (evidence `unit-7aprcjji`), including the new read-only-classifier regression | Green; frozen candidate qualified |
| Joint financial/telemetry smoke | 4/4 passed, zero skips, clean tree at `5e31bacf` (evidence `smoke-ngkx9evx`) | Green; frozen candidate qualified |
| Live client compatibility | 14/14 passed with `neo4j==4.4.6`, zero skips, clean tree at `5e31bacf` (evidence `clients-5odtbvuq`) | Green; preserve dependency manifest and warning record |
| HA series failover | 2/2 passed with zero skips/failures on the fixed candidate and again on the clean frozen candidate `5e31bacf` (evidence `ha-aha_pbu5`, then `ha-2k7e6e4c`); root cause was a product defect, not a harness defect (see workstream A) | Green; frozen candidate qualified |
| Docs and release metadata | `python3 ci/release/check_docs.py` passes; `git diff --check` clean | Run the docs gate after all documentation edits |
| Package and container images | Not qualified | Build, inspect, smoke-test, checksum, and publish only after approval gates |
| GitHub release | Not published | Tag and publish only from the approved immutable commit |

The former HA blocker is resolved by a product fix, not by weakening the
oracle: a standalone `CALL <mutating-procedure> ... YIELD ... RETURN ...`
statement parses as a regular query whose in-query call the v1 read-only
decider ignored, so HA executed it leader-local without raft replication and
followers diverged permanently (exact evidence: raft commit index did not
advance for `CALL db.createSeriesField(...) YIELD field RETURN field`, while
the same statement without trailing `RETURN` advanced it on all replicas).
Fixed in `src/cypher/parser/clause.h` (`QueryPart::ReadOnly` now checks
`iq_call_clause` exactly like `sa_call_clause`); regression coverage is
`TestSeriesTransaction.MutatingProcedureCallsWithReturnClassifyAsWrites`.
The gate still proves committed series values reconcile on every physical
replica after leader loss and restart, with zero skips and zero failures.

The latest HA failure is a release blocker. Do not convert it to a skip or
weaken the oracle. The test must prove that committed series values remain
reconcilable on every physical replica after leader loss and restart.

## Delegated workstreams

### A. HA qualification and product diagnosis

Owner: HA/test agent.

Status 2026-09-22: DONE on the fixed candidate. Diagnosis separated two
issues, and both are fixed without weakening the oracle:

1. Harness routing defect (fixed, test-only): `cypher_on_leader` pinned the
   first live node and never rotated, so any transient login failure or
   leadership change produced 90 s of `Not a leader` against a follower.
   Graph creation now goes through explicit leader discovery plus
   `callCypherToLeader` on a leader-connected client
   (`test_merge_series_ha.py: leader_write/create_graph_on_leader`), with a
   bounded wait until the new graph is listed before seeding
   (`wait_for_graph_visible`). `cypher_on_leader` in `ha_util.py` now rotates
   across all three nodes and logs clients out after each attempt.
2. Product replication defect (fixed, engine): see the gate-status note
   above; fix in `src/cypher/parser/clause.h`, regression test
   `TestSeriesTransaction.MutatingProcedureCallsWithReturnClassifyAsWrites`.

Final gate result: both HA cases pass with zero
skips and zero failures on the fixed candidate (evidence
`/tmp/graphfin-merge-results/ha-aha_pbu5`) and again on the clean frozen
candidate `5e31bacf` with empty dirty manifest
(`/tmp/graphfin-merge-results/ha-2k7e6e4c`, manifest + `results.xml` +
`run.log` + per-node server logs). The passing
result includes: initial writes, exact range verification on all replicas,
leader kill, verification through the new leader, restart of the old leader,
and final per-replica reconciliation.

1. Inspect the latest HA evidence and reproduce the failure with the isolated
   runner: `bash ci/merge/run.sh ha`.
2. Separate harness defects from GraphFin defects. Check graph creation,
   leader discovery, retry deadlines, named-graph catalog propagation, and
   physical-replica reads.
3. Keep the financial and telemetry cases. A passing result must include:
   initial writes, exact range verification on all replicas, leader kill,
   verification through the new leader, restart of the old leader, and final
   per-replica reconciliation.
4. Add or update focused regression coverage for the discovered cause.
5. Report logs, XML, manifest, source hash, binary hashes, container image ID,
   and the final test count. Required result: both HA cases pass with zero
   skips and zero failures.

Dependency: the merged binaries must be rebuilt if source changes. This work
must finish before release packaging or publication.

### B. Final merge qualification

Owner: qualification agent.

1. Run the docs check and Python syntax checks.
2. Run the strict isolated gates on one frozen candidate commit:
   `unit`, `smoke`, `clients`, and `ha`.
3. Provision and record the exact supported client dependency versions. A
   required skip fails the gate.
4. Archive manifests, JUnit XML, logs, image IDs, CMake cache identity, and
   binary/source hashes under a reviewable result directory.
5. Compare results with `docs/testing/post-merge.md`. Mark every release-scope
   case as pass, fail, blocked, or out of scope with a reason.

Required result: one immutable candidate manifest with no unexplained failure,
required skip, stale artifact, or source/binary mismatch.

### C. Performance and rebuild efficiency

Owner: build/performance agent.

1. Validate the incremental rebuild path used by `ci/phase0/build.sh` and the
   isolated merge runner. Measure clean, incremental-source, and docs-only
   rebuilds.
2. Identify timestamp/clock-skew causes and prevent stale artifacts from being
   accepted as fresh evidence.
3. Keep the pinned image and existing cache strategy; do not silently change
   compiler, ABI, build flags, or engine compatibility.
4. Document when a partial rebuild is safe and which source changes require a
   clean rebuild.

Required result: a short report with commands, elapsed time, rebuilt targets,
cache identity, and a recommendation for CI versus local development.

### D. Package and installation qualification

Owner: packaging agent.

1. Finalize fork-specific package metadata for GraphFin while retaining the
   `lgraph_*` executable/API compatibility names and TuGraph engine markers.
2. Build source/runtime packages for the supported architecture and record all
   runtime dependencies, notices, checksums, and build provenance.
3. Test clean installation, startup, authentication, the financial example,
   the telemetry example, restart, and backup/restore where in scope.
4. Ensure package names and documentation use `GraphFin 0.1.0-alpha` and do not
   claim unsupported distributed forwarding, online migration, or history APIs.

Required result: installable artifacts and a reproducible package manifest.

### E. Container image publication

Owner: container/release agent.

1. Define the canonical registry and image names before pushing. Prefer the
   repository-owned GHCR namespace unless the release owner specifies another
   registry.
2. Build the runtime image from the approved package or approved runtime
   binaries. Do not use an upstream TuGraph image as evidence that it contains
   the merged GraphFin features.
3. Publish immutable version tags, at minimum `0.1.0-alpha` and a commit tag;
   add `latest` only if the release owner explicitly wants a moving tag.
4. Run a container smoke test against the published digest and record the
   digest, platform, base image, package checksum, and startup/query output.

Required result: published image digests and a pull-and-run verification from a
clean environment. Do not push an image if the HA or package gate is red.

### F. GitHub release publication

Owner: release agent.

1. Confirm the final candidate commit and all artifact checksums.
2. Create annotated tag `v0.1.0-alpha` only after the qualification manifest
   is approved. Never move or overwrite the tag.
3. Push the tag and create a prerelease titled `GraphFin v0.1.0-alpha`.
4. Attach release notes, packages, checksums, provenance, compatibility limits,
   and links to the qualification evidence.
5. Verify the release page, tag target, downloadable assets, and container
   image digests from an independent read path.

Required result: a GitHub prerelease that points to the approved commit and
contains only artifacts built from that commit.

### G. Documentation and product review

Owner: documentation agent.

1. Keep `README.md`, `README_CN.md`, `docs/README.md`, `docs/product.md`,
   `docs/getting-started.md`, `RELEASE.md`, and this plan consistent.
2. Keep financial and telemetry examples aligned with the actual API surface.
3. Mark inherited TuGraph manuals as historical/reference material and avoid
   rewriting them as current GraphFin promises unless separately reviewed.
4. Check all local links, code blocks, product/version references, and image
   paths.

Required result: `python3 ci/release/check_docs.py` passes and a reviewer can
follow the README to a working local example without undocumented steps.

## Delegation order

1. HA diagnosis and fix.
2. Rebuild the merged binaries and run the final qualification gates.
3. Package and container qualification.
4. Documentation and release-note freeze.
5. Independent release review.
6. Tag, publish GitHub prerelease, publish container images, and verify them.

Packaging and publication are dependent on the qualification result. Agents may
prepare scripts and artifacts before that point, but they must not publish a
tag, GitHub release, or container image while a release-blocking gate is red.

## Final sign-off criteria

Sign-off requires all of the following:

- unit, smoke, client, and HA gates pass on the same frozen candidate;
- no required test is skipped and all failure scope is documented;
- package install/startup/example/restart checks pass;
- container pull-and-run checks pass from the published digest;
- documentation and metadata checks pass;
- release notes state the actual alpha limitations;
- the tag, GitHub release, packages, and image digests resolve to the same
  approved source/build evidence.

Until those conditions hold, the correct status is **GraphFin `0.1.0-alpha`
qualification in progress; not signed off**.
