#!/usr/bin/env python3
# Infra v3 (C1): resolve a cell selection into matrix rows for ci-dispatch.yml.
#
# Usage:
#   resolve-cells.py <cells-txt-or-dash> <scope> [--registry path]
#     cells-txt: newline-separated cell ids (from select-cells.sh / profile),
#                or '-' to read ids from stdin.
#
# Output: JSON array of {"cell": id, "runs-on": ..., "scope": ...} on stdout;
# stats on stderr. Exit 1 on unknown cells.
import json
import os
import sys


def main() -> int:
    args = sys.argv[1:]
    if len(args) < 2:
        print("usage: resolve-cells.py <cells-txt|- > <scope> [--registry PATH]", file=sys.stderr)
        return 2
    registry = "tests/ci/config.json"
    rest = args[2:]
    if "--registry" in rest:
        i = rest.index("--registry")
        registry = rest[i + 1]
    cells_txt, scope = args[0], args[1]
    if cells_txt == "-":
        ids = [l.strip() for l in sys.stdin if l.strip()]
    else:
        ids = [l for l in cells_txt.splitlines() if l.strip()]
    reg = json.load(open(registry))
    by_id = {c["id"]: c for c in reg["cells"]}
    missing = [i for i in ids if i not in by_id]
    if missing:
        print(f"dispatch-error: unknown cells: {missing}", file=sys.stderr)
        return 1
    rows = []
    for cid in ids:
        c = by_id[cid]
        rows.append({"cell": cid, "runs-on": c.get("runs-on", "ubuntu-24.04"),
                     "scope": scope, "coverage": bool(c.get("coverage", False))})
    print(f"resolved {len(rows)} cells, scope={scope}", file=sys.stderr)
    print(json.dumps(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())