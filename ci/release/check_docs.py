#!/usr/bin/env python3
"""Validate product identity, bilingual examples, and current-document local links."""
import ast
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
DOCS = ["README.md", "README_CN.md", "RELEASE.md", "docs/README.md", "docs/product.md",
        "docs/getting-started.md", "docs/roadmap.md", "docs/architecture/README.md",
        "docs/testing/post-merge.md", "release/notes/0.1.0-alpha.md"]


def main():
    errors = []
    product = json.loads((ROOT / "PRODUCT.json").read_text())
    version = (ROOT / product["version_file"]).read_text().strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", version):
        errors.append("VERSION is not a supported semantic version")
    for name in DOCS:
        path = ROOT / name
        text = path.read_text()
        for target in re.findall(r"\]\(([^)]+)\)", text):
            if "://" in target or target.startswith(("#", "mailto:")):
                continue
            target = target.split("#", 1)[0]
            if target and not (path.parent / target).exists():
                errors.append("%s: broken local link %s" % (name, target))
    en = (ROOT / "README.md").read_text()
    cn = (ROOT / "README_CN.md").read_text()
    for text in (en, cn):
        if not text.startswith("# " + product["name"] + "\n") or version not in text:
            errors.append("README product/version does not match metadata")
    for language in ("cypher", "bash"):
        # Ignore translated shell comments but keep executable statements equal.
        blocks = []
        for text in (en, cn):
            blocks.append(["\n".join(line for line in block.splitlines()
                                      if not line.lstrip().startswith("#"))
                           for block in re.findall(r"```" + language + r"\n(.*?)```", text, re.S)])
        if blocks[0] != blocks[1]:
            errors.append("README examples differ: " + language)
    for name in ["ci/merge/run.py", "test/integration/test_merge_series.py",
                 "test/integration/test_merge_series_ha.py"]:
        ast.parse((ROOT / name).read_text(), filename=name)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Product identity, bilingual examples, local links and Python syntax: PASS")
    print("This validates documentation, not release readiness.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
