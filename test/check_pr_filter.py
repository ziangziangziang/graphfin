#!/usr/bin/env python3
"""Regression check for the pr_engine_gate change filter in .github/workflows/ci.yml.

Covers engine sources, headers, tests, suite configuration, CI scripts,
dependencies, root/nested CMake files, the workflow itself, docs-only and
site-only changes, plus fail-closed behavior on diff failure.

Single source of truth: the pattern and the step logic are extracted from the
workflow file at test time, never duplicated here. A filter edit that breaks
coverage fails this check instead of silently letting engine PRs skip the gate.
"""
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
WORKFLOW = REPO / ".github/workflows" / "ci.yml"

MUST_TRIGGER = [
    "src/server/lgraph_server.cpp",      # engine source
    "src/core/lmdb_store.h",             # engine header
    "test/test_kv_store.cpp",            # engine test
    "test/suites.json",                  # suite configuration
    "test/check_results.py",             # gate script
    "ci/github_pr_test.sh",              # CI script
    "ci/images/github-pr-ubuntu-latest-Dockerfile",  # CI image definition
    "deps/build_deps.sh",                # dependency build
    "CMakeLists.txt",                    # root CMake
    "Options.cmake",                     # root cmake include (see CMakeLists.txt:7)
    "FindOpenSSL.cmake",                 # root cmake module
    "src/core/CMakeLists.txt",           # nested CMakeLists
    "cmake/FindSomething.cmake",         # nested cmake module
    ".github/workflows/ci.yml",          # the gate itself
]

MUST_NOT_TRIGGER = [
    "README.md",
    "docs/roadmap.md",
    "docs/architecture/07-scalability-risks.md",
    "site/src/main.ts",
    "site/package.json",
    "AGENTS.md",
]

FAILURES = []


def check(name, cond, detail=""):
    print(("PASS " if cond else "FAIL ") + name + (" -- " + detail if detail and not cond else ""))
    if not cond:
        FAILURES.append(name)


def extract_detect_step(text):
    """Return (grep_pattern, step_script) from the Detect step."""
    lines = text.splitlines()
    start = next(i for i, l in enumerate(lines) if "Detect build-relevant changes" in l)
    run_at = next(i for i in range(start, len(lines)) if re.match(r"\s*run: \|\s*$", lines[i]))
    indent = len(lines[run_at]) - len(lines[run_at].lstrip())
    body = []
    for line in lines[run_at + 1:]:
        if line.strip() == "":
            body.append("")
            continue
        cur = len(line) - len(line.lstrip())
        if cur <= indent:
            break
        body.append(line[indent + 2:])
    script = "\n".join(body)
    m = re.search(r"grep -qE '([^']+)'", script)
    if not m:
        sys.exit("check_pr_filter: could not extract grep pattern from Detect step")
    return m.group(1), script


def run_step(script, base, head, cwd):
    """Execute the extracted step with given SHAs; return GITHUB_OUTPUT content."""
    script = script.replace("${{ github.event.pull_request.base.sha }}", base)
    script = script.replace("${{ github.event.pull_request.head.sha }}", head)
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "output")
        Path(out).touch()
        proc = subprocess.run(["bash", "-c", script], cwd=cwd,
                              env={**os.environ, "GITHUB_OUTPUT": out},
                              capture_output=True, text=True, timeout=60)
        return out, proc, Path(out).read_text()


def git(repo, *args):
    subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t",
                    *args], cwd=repo, check=True, capture_output=True, text=True)


def main():
    text = WORKFLOW.read_text()
    pattern, script = extract_detect_step(text)
    print("pattern under test: %s" % pattern)
    try:
        rx = re.compile(pattern)
    except re.error as exc:
        sys.exit("check_pr_filter: pattern does not compile: %s" % exc)

    for path in MUST_TRIGGER:
        check("trigger:" + path, rx.search(path) is not None)
    for path in MUST_NOT_TRIGGER:
        check("skip:" + path, rx.search(path) is None)

    # The step diffs SHAs, so exercise it in a scratch repo: engine change
    # must trigger, docs-only must skip, and -- critically -- an undiffable
    # pair must trigger (fail closed) rather than read as "no changes".
    with tempfile.TemporaryDirectory() as tmp:
        repo = os.path.join(tmp, "r")
        os.mkdir(repo)
        git(repo, "init", "-q")
        Path(repo, "README.md").write_text("base\n")
        git(repo, "add", "README.md")
        git(repo, "commit", "-qm", "base")
        base = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo,
                              check=True, capture_output=True, text=True).stdout.strip()

        eng = Path(repo, "src", "server")
        eng.mkdir(parents=True)
        Path(eng, "x.cpp").write_text("v2\n")
        git(repo, "add", "src/server/x.cpp")
        git(repo, "commit", "-qm", "engine")
        eng_head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo,
                                  check=True, capture_output=True, text=True).stdout.strip()

        _, _, out = run_step(script, base, eng_head, repo)
        check("step:engine-change-triggers", "engine=true" in out, out.strip())

        Path(repo, "docs").mkdir(exist_ok=True)
        # Branch from base so the diff contains ONLY the docs change; a docs
        # commit stacked on the engine commit would correctly trigger.
        git(repo, "checkout", "-qb", "docs-only", base)
        Path(repo, "docs", "y.md").write_text("docs\n")
        git(repo, "add", "docs/y.md")
        git(repo, "commit", "-qm", "docs")
        docs_head = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo,
                                   check=True, capture_output=True, text=True).stdout.strip()
        _, _, out = run_step(script, base, docs_head, repo)
        check("step:docs-only-skips", "engine=false" in out, out.strip())

        _, _, out = run_step(script, "d34db33f" * 5, "d34db33f" * 5, repo)
        check("step:diff-failure-triggers", "engine=true" in out, out.strip())

    if FAILURES:
        print("check_pr_filter: %d FAILURE(S)" % len(FAILURES))
        return 1
    print("check_pr_filter: all %d assertions passed" %
          (len(MUST_TRIGGER) + len(MUST_NOT_TRIGGER) + 3))
    return 0


if __name__ == "__main__":
    sys.exit(main())
