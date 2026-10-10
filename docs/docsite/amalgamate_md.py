#!/usr/bin/env python3
"""Amalgamated-markdown tooling for published libmdbx docs.

strip_dist_cutoff(text): removes `<!-- dist-cutoff-begin --> ... <!--
dist-cutoff-end -->` regions (the same marker convention as `make dist` uses
for C sources) and validates marker pairing.

audit_amalgamated_refs(md_path, package_root): published documents must be
applicable to the amalgamated package - every repo file they reference in
markdown links must exist there (or be explicitly allow-listed as a dev-only
pointer). Returns a list of violations.
"""
import re
import sys
from pathlib import Path

BEGIN = "<!-- dist-cutoff-begin -->"
END = "<!-- dist-cutoff-end -->"
# dev-only docs/eng helpers that published pages may mention as pointers
# (they are NOT part of the amalgamated package but referencing them is a
# conscious editorial decision, not an accident)
ALLOW_DEV = {"docs/engineering/", "skynet/", "tests/", "CHANGELOG"}


def strip_dist_cutoff(text: str, origin: str = "") -> str:
    out, pos, cuts = [], 0, 0
    while True:
        b = text.find(BEGIN, pos)
        if b < 0:
            out.append(text[pos:])
            break
        e = text.find(END, b)
        if e < 0:
            raise ValueError(f"{origin}: unclosed {BEGIN}")
        out.append(text[pos:b])
        pos = e + len(END)
        cuts += 1
    if text.count(BEGIN) != text.count(END):
        raise ValueError(f"{origin}: unbalanced dist-cutoff markers")
    return "".join(out), cuts


MD_LINK = re.compile(r"\[[^\]]*\]\(([^)#\s]+)(?:#[^)]*)?\)")


def audit_amalgamated_refs(md_path: Path, package_root: Path):
    """Report markdown links pointing to files that do not exist in the
    amalgamated package. Only repo-relative links are checked; http(s) and
    pure anchors are skipped."""
    violations = []
    text = md_path.read_text(encoding="utf-8")
    for m in MD_LINK.finditer(text):
        target = m.group(1)
        if target.startswith(("http://", "https://", "mailto:")):
            continue
        # resolve relative to the doc location, then try to map into the repo
        rel = (md_path.parent / target).resolve()
        candidates = [rel, package_root / target,
                      package_root / rel.relative_to(md_path.parents[2])
                      if md_path.parents[2] in rel.parents else None]
        ok = any(c and c.exists() for c in candidates if c)
        if not ok:
            violations.append(f"{md_path.name}: {target}")
    return violations


PUBLISHED = ("architecture.ru.md", "architecture.en.md", "deep-dive.ru.md",
             "deep-dive.en.md", "improvements.ru.md", "improvements.en.md")

if __name__ == "__main__":
    pkg = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    bad = 0
    for md in sorted(pkg.glob("docs/engineering/*.md")):
        if md.name not in PUBLISHED:
            continue  # dev-only docs are not published; skip the audit
        try:
            _, cuts = strip_dist_cutoff(md.read_text(encoding="utf-8"), str(md))
        except ValueError as e:
            print(f"  MARKER FAIL: {e}")
            bad += 1
            continue
        if cuts:
            print(f"  {md.name}: {cuts} dist-cutoff regions")
        for v in audit_amalgamated_refs(md, pkg):
            print(f"  REF FAIL: {v}")
            bad += 1
    sys.exit(1 if bad else 0)
