#!/usr/bin/env python3
"""Generate the legacy /doxygen/*.html -> new docsite redirect table.

The legacy doxygen site (docs/html, built by `make doxygen`) is replaced by
the unified docsite (docs/docsite, built by `make docsite`). The owner wires
the actual HTTP redirects; this script produces the mapping file:

    docs/docsite/doxygen-redirects.tsv   (tab-separated: old-path<TAB>new-path)

Rules (checked in order, first match wins):
  1. index.html                  -> /docs/en/ (the docsite landing)
  2. _change_log_8md.html        -> /docs/en/ChangeLog.html
  3. exact match                 -> /docs/en/api/<name>.html
  4. <refid>-members.html        -> /docs/en/api/<refid>.html
  5. functions_*  member indexes -> the matching mkdoxy member index page
  6. graph_legend.html           -> /docs/en/api/hierarchy.html
  7. concepts / doxygen_crawl    -> dropped (crawl artifact) or annotated index

Usage: python3 gen_doxygen_redirects.py [doxygen-html-dir] [api-dir]
       (defaults: docs/html, build/docsite/en/api)
"""
import os
import re
import sys
from pathlib import Path

_root = Path(__file__).resolve().parent.parent.parent
OLD_DIR = Path(sys.argv[1]) if len(sys.argv) > 1 else _root / "docs" / "html"
API_DIR = Path(sys.argv[2]) if len(sys.argv) > 2 else _root / "build" / "docsite" / "en" / "api"
OUT = Path(__file__).resolve().parent / "doxygen-redirects.tsv"

BASE = "/docs/en/api"


def functions_target(name: str):
    """Map doxygen's per-letter member index pages onto the mkdoxy
    aggregate member index pages."""
    if re.match(r"^functions_func", name):
        return f"{BASE}/class_member_functions.html"
    if re.match(r"^functions_type", name) or re.match(r"^functions_(enum|enumval)", name):
        return f"{BASE}/class_member_enums.html"
    if re.match(r"^functions_(vars|prop|event)", name):
        return f"{BASE}/class_member_variables.html"
    if re.match(r"^functions_(reimp|friend|eval|static)", name):
        return f"{BASE}/class_members.html"
    if re.match(r"^functions(_[a-z~]+)?$", name):
        return f"{BASE}/class_members.html"
    return None


def main():
    old_pages = sorted(p.name for p in OLD_DIR.glob("*.html"))
    new_pages = {p.name for p in API_DIR.glob("*.html")}
    if not old_pages:
        raise SystemExit(f"no .html found in {OLD_DIR} - run `make doxygen` first")
    if not new_pages:
        raise SystemExit(f"no .html found in {API_DIR} - run `make docsite` first")

    rows = []
    dropped = []
    for name in old_pages:
        old_path = f"/doxygen/{name}"
        if name == "index.html":
            rows.append((old_path, "/docs/en/"))
            continue
        if name == "_change_log_8md.html":
            rows.append((old_path, "/docs/en/ChangeLog.html"))
            continue
        if name in new_pages:
            rows.append((old_path, f"{BASE}/{name}"))
            continue
        m = re.match(r"^(.+)-members\.html$", name)
        if m and f"{m.group(1)}.html" in new_pages:
            rows.append((old_path, f"{BASE}/{m.group(1)}.html"))
            continue
        m = re.match(r"^(pages|pages_.+)\.html$", name)
        if m and f"{m.group(1)}.html" in new_pages:
            rows.append((old_path, f"{BASE}/{name}"))
            continue
        if name == "graph_legend.html" or re.match(r"^(class|hierarchy)\.html$", name):
            rows.append((old_path, f"{BASE}/hierarchy.html"))
            continue
        if name == "topics.html":
            rows.append((old_path, f"{BASE}/modules.html"))
            continue
        if name == "md__change_log.html":
            rows.append((old_path, "/docs/en/ChangeLog.html"))
            continue
        # doxygen global/namespace symbol indexes -> mkdoxy aggregate indexes
        symbol_index = {
            "globals.html": "files.html",
            "globals_defs.html": "macros.html",
            "globals_func.html": "functions.html",
            "globals_eval.html": "functions.html",
            "globals_type.html": "variables.html",
            "globals_enum.html": "variables.html",
            "globals_vars.html": "variables.html",
            "namespacemembers.html": "namespace_members.html",
            "namespacemembers_eval.html": "namespace_members.html",
            "namespacemembers_func.html": "namespace_member_functions.html",
            "namespacemembers_type.html": "namespace_member_typedefs.html",
            "namespacemembers_enum.html": "namespace_member_enums.html",
            "namespacemembers_vars.html": "namespace_member_variables.html",
        }
        if name in symbol_index:
            rows.append((old_path, f"{BASE}/{symbol_index[name]}"))
            continue
        # per-letter splits of the global index -> the aggregate page
        if re.match(r"^globals_[a-z~]+\.html$", name):
            rows.append((old_path, f"{BASE}/files.html"))
            continue
        if name == "concepts.html" or name.startswith("concept"):
            rows.append((old_path, f"{BASE}/annotated.html"))
            continue
        if functions_target(name[:-5] if name.endswith(".html") else name):
            rows.append((old_path, functions_target(name[:-5])))
            continue
        if name == "doxygen_crawl.html" or name.endswith(".js") or name.endswith(".css"):
            dropped.append(name)
            continue
        dropped.append(name)

    with open(OUT, "w", encoding="utf-8") as f:
        f.write("# Legacy /doxygen/*.html -> unified docsite redirect table.\n")
        f.write("# Generated by docs/docsite/gen_doxygen_redirects.py - do not edit by hand;\n")
        f.write("# regenerate after API changes with: make doxygen && make docsite &&\n")
        f.write("#   python3 docs/docsite/gen_doxygen_redirects.py\n")
        f.write("# Format: <old-path>\\t<new-path> (tab-separated, one redirect per line).\n")
        for old, new in rows:
            f.write(f"{old}\t{new}\n")

    print(f"  redirects: {len(rows)} mappings -> {OUT}")
    if dropped:
        print(f"  dropped (no target): {len(dropped)}:")
        for name in dropped:
            print(f"    {name}")


if __name__ == "__main__":
    main()
