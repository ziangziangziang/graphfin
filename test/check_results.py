#!/usr/bin/env python3
"""Validate a gtest XML result against its suite contract in test/suites.json.

Usage:
  test/check_results.py --suite NAME --xml PATH [--artifacts DIR]

A suite passes only when it produced cases, met its min_cases floor, had zero
failures and zero skips, and every required_artifacts binary is present. Zero
selection, a partial run, and a silently skipped required case are all failures:
a green badge must never be reachable by running less than the contract asks for.
"""
import argparse
import fnmatch
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, str(Path(__file__).resolve().parent))
import suites as suite_catalog


def tally(doc):
    cases = list(doc.iter("testcase"))
    failures = sum(any(e.tag in ("failure", "error") for e in c) for c in cases)
    skipped = sum(len(list(c.iter("skipped"))) + (c.get("status") == "notrun")
                  for c in cases)
    assertions = sum(len(list(c.iter("failure"))) + len(list(c.iter("error")))
                     for c in cases)
    return cases, failures, skipped, assertions


def case_name(case):
    """Full gtest identity, i.e. the "Suite.Test" form a filter uses."""
    name = case.get("name", "")
    suite = case.get("classname")
    return "%s.%s" % (suite, name) if suite and "." not in name else name


def missing_literals(gtest_filter, cases):
    """Literal (wildcard-free) filter patterns that matched nothing.

    A filter is a union, so an aggregate case count happily passes when one
    requested test was renamed or typo'd and silently stopped running. Every
    exact pattern named in the catalog must be present in the result.
    """
    seen = {case_name(c) for c in cases}
    missing = []
    for pattern in gtest_filter.split(":"):
        pattern = pattern.strip()
        if not pattern or "*" in pattern or "?" in pattern:
            continue
        if pattern not in seen:
            missing.append(pattern)
    return missing


def unmatched_patterns(gtest_filter, cases):
    """Filter patterns (wildcard or literal) that selected zero cases.

    Literals are delegated to missing_literals; this covers the wildcard
    half: a pattern such as TestSeries* must actually match something, or a
    silently empty core family passes on the back of unrelated tests.
    gtest `*`/`?` behave like fnmatch over the full "Suite.Test" name.
    """
    names = [case_name(c) for c in cases]
    unmatched = []
    for pattern in gtest_filter.split(":"):
        pattern = pattern.strip()
        if not pattern or pattern.startswith("-"):
            continue
        if "*" not in pattern and "?" not in pattern:
            continue
        if not any(fnmatch.fnmatchcase(n, pattern) for n in names):
            unmatched.append(pattern)
    return unmatched


def missing_families(required_families, cases):
    """Mandatory suite families with zero executed cases.

    required_families names gtest fixtures (the classname in the XML). Each
    must contribute at least one case: without this, an XML containing only
    the small named regression tests passes while the entire series, cluster,
    router, shard, and migration coverage is absent.
    """
    present = {c.get("classname", "") for c in cases}
    return [f for f in required_families if f not in present]


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite", required=True)
    parser.add_argument("--xml", required=True)
    parser.add_argument("--artifacts", help="directory holding required_artifacts")
    args = parser.parse_args(argv[1:])

    data = suite_catalog.load()
    suite_catalog.validate(data)
    suite = data["suites"].get(args.suite)
    if suite is None:
        sys.exit("check_results: unknown suite: %s" % args.suite)
    if suite["type"] != "gtest":
        sys.exit("check_results: %s is not a gtest suite" % args.suite)

    xml = Path(args.xml)
    if not xml.is_file():
        sys.exit("check_results: no result XML at %s" % xml)
    cases, failures, skipped, assertions = tally(ET.parse(str(xml)).getroot())

    minimum = suite["min_cases"]
    print("%s: %d cases, %d assertion failures, %d failed, %d skipped (min_cases=%d)"
          % (args.suite, len(cases), assertions, failures, skipped, minimum))

    if not cases:
        sys.exit("check_results: %s selected zero tests" % args.suite)
    if failures:
        sys.exit("check_results: %s has %d failed cases" % (args.suite, failures))
    if skipped:
        sys.exit("check_results: %s has %d skipped cases" % (args.suite, skipped))
    if len(cases) < minimum:
        sys.exit("check_results: %s ran %d cases, below min_cases=%d"
                 % (args.suite, len(cases), minimum))

    absent = missing_literals(suite.get("gtest_filter", ""), cases)
    if absent:
        sys.exit("check_results: %s did not run catalog-listed test(s): %s"
                 % (args.suite, ", ".join(absent)))

    unmatched = unmatched_patterns(suite.get("gtest_filter", ""), cases)
    if unmatched:
        sys.exit("check_results: %s filter pattern(s) selected zero cases: %s"
                 % (args.suite, ", ".join(unmatched)))

    missing = missing_families(suite.get("required_families", []), cases)
    if missing:
        sys.exit("check_results: %s ran zero cases from mandatory family/ies: %s"
                 % (args.suite, ", ".join(missing)))

    if args.artifacts:
        out = Path(args.artifacts)
        for name in suite.get("required_artifacts", []):
            if not (out / name).exists():
                sys.exit("check_results: %s missing required artifact %s"
                         % (args.suite, name))

    print("%s: PASSED" % args.suite)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
