#!/usr/bin/env python3
"""Strict, isolated post-merge gate. Run inside the pinned compile container.

Example: python3 /workspace/ci/merge/run.py --suite smoke --out /results
No build, pip install, database cleanup outside its own scratch tree, or skip waiver.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "test"))
import suites as suite_catalog

# Selection from test/suites.json (suites merge-unit/smoke/clients/ha).
# suites.py validates the catalog on load; empty selections fail loudly.
_catalog = suite_catalog.load()
suite_catalog.validate(_catalog)
FILTER = _catalog["suites"]["merge-unit"]["gtest_filter"]
PYTEST_FILES = {k.split("-", 1)[1]: v["pytest_files"][0]
                for k, v in _catalog["suites"].items()
                if k.startswith("merge-") and v["type"] == "pytest"}
MINIMUM = {k.split("-", 1)[1]: v["min_cases"]
           for k, v in _catalog["suites"].items() if k.startswith("merge-")}
REQUIRED_FAMILIES = _catalog["suites"]["merge-unit"]["required_families"]


def sha(path):
    h = hashlib.sha256()
    with open(str(path), "rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def git(*args):
    return subprocess.check_output(["git"] + list(args), cwd=str(ROOT)).decode().strip()


def source_state():
    names = subprocess.check_output([
        "git", "ls-files", "-z", "--modified", "--others",
        "--exclude-standard"], cwd=str(ROOT)).decode().split("\0")
    # Include staged files even if they match the working tree.
    names += subprocess.check_output([
        "git", "diff", "--cached", "--name-only", "-z"], cwd=str(ROOT)).decode().split("\0")
    return {"commit": git("rev-parse", "HEAD"),
            "dirty": {n: sha(ROOT / n) if (ROOT / n).is_file() else "deleted"
                      for n in sorted(set(names)) if n}}


def dependency_hash(path):
    root = Path(path)
    h = hashlib.sha256()
    for p in sorted(root.rglob("*")):
        if p.is_file():
            h.update(str(p.relative_to(root)).encode() + b"\0" + sha(p).encode())
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=["unit", "smoke", "clients", "ha"], required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    # Unique run paths prevent old XML from being accepted after an early crash.
    run = Path(tempfile.mkdtemp(prefix=args.suite + "-", dir=str(out)))
    scratch = Path(tempfile.mkdtemp(prefix="merge-gate-"))
    stage = scratch / "repo"
    output = stage / "build" / "output"
    tests = stage / "test" / "integration"
    output.mkdir(parents=True)
    tests.mkdir(parents=True)
    before = source_state()
    binaries = (["unit_test", "liblgraph.so"] if args.suite == "unit" else
                ["lgraph_server", "liblgraph.so", "liblgraph_client_python.so"])
    if args.suite == "clients":
        binaries.append("lgraph_backup")
    manifest = {"suite": args.suite, "source": before, "started_epoch": time.time(),
                "image_id": os.environ.get("MERGE_IMAGE_ID", "unrecorded"),
                "memory_limit": os.environ.get("MERGE_MEMORY", "unrecorded"),
                "status": "FAILED", "binaries": {}}
    try:
        for name in binaries:
            original = ROOT / "build" / "output" / name
            manifest["binaries"][name] = sha(original)
            (output / name).symlink_to(original)
        cache = ROOT / "build" / "CMakeCache.txt"
        manifest["cmake_cache_sha256"] = sha(cache)
        shutil.copy2(str(cache), str(run / "CMakeCache.txt"))
        (stage / "test" / "resource").symlink_to(ROOT / "test" / "resource")
        for name in ["lgraph_standalone.json", "lgraph_ha.json"]:
            shutil.copy2(str(ROOT / "build" / "output" / name), str(output / name))
        for name in ["test_merge_series.py", "test_merge_series_ha.py", "test_timeseries.py",
                     "phase0_util.py", "ha_util.py", "bolt_driver.py"]:
            shutil.copy2(str(ROOT / "test" / "integration" / name), str(tests / name))
        env = dict(os.environ)
        env["PYTHONPATH"] = ":".join([str(ROOT / "build/output"), str(tests),
                                     str(ROOT / "src/client/python/TuGraphClient")])
        dependencies = env.get("MERGE_PYTHON_DEPS")
        if dependencies:
            env["PYTHONPATH"] = dependencies + ":" + env["PYTHONPATH"]
            manifest["python_dependencies_sha256"] = dependency_hash(dependencies)
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        env["PIP_NO_INDEX"] = "1"
        env["LD_LIBRARY_PATH"] = str(ROOT / "build/output") + ":" + env.get("LD_LIBRARY_PATH", "")
        if args.suite == "clients" and dependencies:
            driver_version = subprocess.check_output(
                [sys.executable, "-c", "import neo4j; print(neo4j.__version__)"], env=env
            ).decode().strip()
            manifest["neo4j_driver_version"] = driver_version
            assert driver_version == "4.4.6", "unexpected Neo4j driver version"
        xml = run / "results.xml"
        if args.suite == "unit":
            command = [str(output / "unit_test"), "--gtest_filter=" + FILTER,
                       "--gtest_output=xml:" + str(xml)]
        else:
            filename = PYTEST_FILES[args.suite]
            command = [sys.executable, "-m", "pytest", str(tests / filename), "-v", "-rs",
                       "--basetemp=" + str(scratch / "pytest"),
                       "--junitxml=" + str(xml)]
        manifest["command"] = command
        with open(str(run / "run.log"), "w") as log:
            result = subprocess.run(command, cwd=str(output), env=env, stdout=log,
                                    stderr=subprocess.STDOUT, timeout=args.timeout)
        manifest["returncode"] = result.returncode
        doc = ET.parse(str(xml)).getroot()
        cases = list(doc.iter("testcase"))
        assertion_failures = sum(len(list(c.iter("failure"))) + len(list(c.iter("error")))
                                 for c in cases)
        failures = sum(any(e.tag in ("failure", "error") for e in c) for c in cases)
        skipped = sum(len(list(c.iter("skipped"))) + (c.get("status") == "notrun") for c in cases)
        manifest.update(tests=len(cases), failures=failures, skipped=skipped,
                        assertion_failures=assertion_failures)
        assert cases and result.returncode == 0 and failures == 0 and skipped == 0, \
            "required tests failed, skipped, or produced no cases"
        minimum = MINIMUM[args.suite]
        assert len(cases) >= minimum, "required case count is incomplete"
        if args.suite == "unit":
            classes = {c.get("classname", "") for c in cases}
            for family in REQUIRED_FAMILIES:
                assert family in classes, "missing compiled suite: " + family
        assert source_state() == before, "source changed during validation; rerun on stable source"
        assert all(sha(ROOT / "build/output" / n) == h
                   for n, h in manifest["binaries"].items()), "binaries changed during validation"
        if dependencies:
            assert dependency_hash(dependencies) == manifest["python_dependencies_sha256"], \
                "Python dependency bundle changed during validation"
        manifest["status"] = "PASSED"
    except Exception as exc:
        manifest["error"] = "%s: %s" % (type(exc).__name__, exc)
    finally:
        manifest["finished_epoch"] = time.time()
        for log in scratch.rglob("*.log"):
            destination = run / "server-logs" / log.relative_to(scratch)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(str(log), str(destination))
        (run / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(json.dumps(manifest, indent=2))
        print("Evidence:", run)
    return 0 if manifest["status"] == "PASSED" else 1


if __name__ == "__main__":
    sys.exit(main())
