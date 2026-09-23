# GraphFin release preparation

Release identity: **GraphFin 0.1.0-alpha**, with Git tag `v0.1.0-alpha`, derived
from TuGraph 4.5.2.
**No release is approved by this document.** The identity is confirmed, but
qualification and publication are still pending. See the qualification record
below and [TASK.md](TASK.md) for delegated work.

## Version and compatibility policy

`VERSION` is the product distribution version. `PRODUCT.json` records
the product identity and upstream compatibility baseline. Do not lower the existing
`LGRAPH_VERSION_*` engine/database version to 0.1.0: those numbers also participate
in database compatibility checks. Keep the `lgraph_*` executable names, SDK
namespaces and on-disk markers until separately reviewed migrations exist.

CPack package metadata is rebranded as GraphFin. Current runtime banners and
CLI prompts still identify TuGraph 4.5.2.
Runtime display metadata must be finalized after identity confirmation;
documentation branding alone is not complete binary
rebranding. Release packages must report both product and engine identity.

Do not advertise downgrade, mixed-version HA, transparent sharding, online
migration, historical revision queries or fixed capacity without explicit support
and corresponding tests. Retain upstream LICENSE and source copyright notices.

## Candidate scope

The first candidate should cover local graph/series functionality and the validated
multi-graph lifecycle surface. HA is supported only if the merged HA gate and the
declared failure/recovery qualification pass. Sharding control-plane components
may be included as experimental internal components; live distributed routing
and migration remain outside the public support claim.

## Qualification record

| Item | Required outcome | Current state |
| --- | --- | --- |
| Source and build identity | One merged commit and recorded configuration; reproducible build | Incremental merged build completed; final frozen-candidate build and provenance still pending |
| Core merge regression | Series + cluster/router/migration/lifecycle tests present and passing | Strict unit gate: 110/110 passed on frozen `5e31bacf` (evidence `unit-7aprcjji`; first passed dirty as `unit-p6uuknon`) |
| Joint lifecycle smoke | Both domain fixtures, tenant isolation, series correction, recreation and snapshot restore | Strict smoke gate: 4/4 passed on frozen `5e31bacf` (evidence `smoke-ngkx9evx`) |
| Clients | Real supported drivers, no required skips | Strict client gate: 14/14 passed with `neo4j==4.4.6` on frozen `5e31bacf` (evidence `clients-5odtbvuq`) |
| HA | Exact per-replica series reconciliation after leader loss, plus declared failure scope | Fixed: strict HA gate 2/2 passed with zero skips/failures on frozen `5e31bacf` (evidence `ha-2k7e6e4c`; first passed dirty as `ha-aha_pbu5`). Root cause was a product defect (v1 read-only decider ignored in-query procedure calls, so `CALL ... YIELD ... RETURN ...` writes executed leader-local without raft replication; fixed in `src/cypher/parser/clause.h`) |
| Reliability and compatibility | Applicable remaining tests in the post-merge matrix | In-flight crash, active-reader pressure, full sanitizers, released fixtures and extended soak remain open |
| Product identity | Owner confirms name/version and public destination | Confirmed: GraphFin `0.1.0-alpha`; publication destination still to be verified |
| Package and installation | Product/engine versions, complete runtime dependencies, checksums, clean install and restore smoke | Not qualified |
| Publication | Tag and release reference exact approved commit/artifact hashes | Not performed |

## Build, validate, package, publish

1. Freeze the candidate source and build configuration. Run one incremental build
   during development; perform the final clean build in an isolated build tree.
   Do not erase the shared development tree or rebuild for each pytest run.
2. Run `bash ci/merge/run.sh unit`, `smoke`, `clients`, and `ha` as applicable.
   Provision the real driver in the selected image first. Archive each run's XML,
   manifest and logs; required skips reject the candidate.
3. Close the release-scope rows in [the test matrix](docs/testing/post-merge.md).
   Agree numeric scale/latency/memory budgets and archive dedicated benchmark and
   soak records. Parent-branch results are not merged-release evidence.
4. Confirm `PRODUCT.json`/`VERSION`, update the bilingual READMEs and release notes,
   and finish product display/package metadata without changing storage identity.
   Run `python3 ci/release/check_docs.py`.
5. Build source and runtime packages from the selected commit, with upstream
   notices and runtime dependencies. Name artifacts with product version, OS and
   architecture. Produce SHA-256 checksums and a build manifest. Validate a clean
   installation, example queries, restart, and an actual backup/restore.
6. Create an annotated candidate tag only after the evidence is reviewed. A tag
   identifies the exact commit; assets must derive from that commit. Publish to
   the confirmed repository/registry, mark an RC as a prerelease, and include
   checksums, compatibility limits and evidence links. Never overwrite a release
   asset or tag to silently change its contents.

The inherited `ci/build_release.sh` and CPack recipes are not yet a
qualified release pipeline. No public download/image URL should be invented in
the READMEs. The manual merge workflow is a validation tool, not publication.
