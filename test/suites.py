#!/usr/bin/env python3
"""Query test/suites.json, the canonical suite catalog.

Usage:
  test/suites.py --suite NAME --field FIELD   print one field (lists: space-joined)
  test/suites.py --validate                   schema-check the whole catalog

Every query validates first: missing suite/field, empty required selection,
or schema violation fails loudly so runners never silently run zero tests.
"""
import json
import sys
from pathlib import Path

CATALOG = Path(__file__).resolve().parent / "suites.json"
TIERS = {"pr", "merge", "main", "dev", "nightly"}
REQUIRED_KEYS = {"description", "tier", "type", "min_cases", "owner"}


def load():
    try:
        data = json.loads(CATALOG.read_text())
    except Exception as exc:
        sys.exit("suites: cannot parse %s: %s" % (CATALOG, exc))
    return data


def validate(data=None):
    data = data if data is not None else load()
    assert isinstance(data.get("version"), int), "version must be int"
    suites = data.get("suites")
    assert isinstance(suites, dict) and suites, "suites must be a non-empty map"
    for name, s in suites.items():
        missing = REQUIRED_KEYS - set(s)
        assert not missing, "%s: missing keys %s" % (name, sorted(missing))
        assert s["tier"] in TIERS, "%s: bad tier %r" % (name, s["tier"])
        assert s["type"] in ("gtest", "pytest"), "%s: bad type %r" % (name, s["type"])
        assert isinstance(s["min_cases"], int) and s["min_cases"] >= 1, \
            "%s: min_cases must be >= 1" % name
        if s["type"] == "gtest":
            assert "gtest_filter" in s, "%s: gtest suite needs gtest_filter" % name
        else:
            assert "pytest_files" in s, "%s: pytest suite needs pytest_files" % name
            assert isinstance(s["pytest_files"], list), "%s: pytest_files must be a list" % name
    excl = data.get("procedure_off_exclusions", [])
    assert isinstance(excl, list) and excl, "procedure_off_exclusions must be non-empty"
    return True


def main(argv):
    if "--validate" in argv:
        validate()
        print("suites: ACCEPT (%s, %d suites)" % (CATALOG, len(load()["suites"])))
        return 0
    try:
        suite = argv[argv.index("--suite") + 1]
        field = argv[argv.index("--field") + 1]
    except (ValueError, IndexError):
        sys.exit("usage: suites.py --suite NAME --field FIELD | --validate")
    data = load()
    validate(data)
    suite_map = data["suites"].get(suite)
    if suite_map is None:
        sys.exit("suites: unknown suite: %s" % suite)
    if field in suite_map:
        value = suite_map[field]
    elif field in data:
        # Catalog-global fallback (e.g. procedure_off_exclusions shared by
        # main-it and the Phase 0 runner); suite values override it.
        value = data[field]
    else:
        sys.exit("suites: unknown suite/field: %s/%s" % (suite, field))
    if isinstance(value, list):
        print(" ".join(value))
    elif isinstance(value, dict):
        print(json.dumps(value))
    else:
        print(value)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
