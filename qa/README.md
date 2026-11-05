# QA

| Path | Purpose |
|---|---|
| `local/` | Pinned-container build, correctness suites, perf/soak profiles and the scaling benchmark. Start with `local/README.md`; the entry point is `local/run.sh`. |

Historical Phase 0 evidence (results, R0 investigation, experiment and
sanitizer sources) lives in `docs/architecture/evidence/phase0/`. The suite
catalog is `test/suites.json`; CI runners and the local harness both query it
through `test/suites.py`.
