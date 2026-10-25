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
                     "scope": scope, "coverage": bool(c.get("coverage", False)),
                     "ndk": c.get("ndk", "")})
    # Order the matrix so the slowest/rarest cells get initiated first. GitHub
    # Actions hands out runners of one label in arrival order and caps the
    # concurrency, so putting the long cells front reduces the makespan when
    # the queue throttles the burst; with plentiful runners this is a no-op
    # (stable sort keeps the original profile order for equals).
    for row in rows:
        c = by_id[row["cell"]]
        row["_expected_sec"] = int(c.get("expected_sec", 0) or 0)
    rows.sort(key=lambda r: r["_expected_sec"], reverse=True)
    for row in rows:
        del row["_expected_sec"]
    print(f"resolved {len(rows)} cells, scope={scope}", file=sys.stderr)
    print(json.dumps(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())