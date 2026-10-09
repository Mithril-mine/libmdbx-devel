#!/usr/bin/env python3
# Infra v3 (C1): diff -> cell selection. Maps changed paths onto the
# path_zones registry (tests/ci/config.json) and prints the union of cell
# ids to run for the fast tier. Combined with run-cell.py --scope fast this
# is the change-addressed push gate: "never run what did not change".
#
# Usage:
#   tests/ci/select-cells.py [--base <ref>] [path...]
#   tests/ci/select-cells.py                    # changed = git diff vs origin/devel
#   tests/ci/select-cells.py src/cursor.c       # explicit paths
#   tests/ci/select-cells.py --profile push-quick   # fixed profile (nightly/full)
#   tests/ci/select-cells.py --cap N            # shrink to N diverse cells
#
# Output: newline-separated cell ids on stdout; stats on stderr.
# Exit: 0 = ok (may be empty), 1 = nothing selected, 2 = usage/error.

import argparse
import json
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
REGISTRY = REPO_ROOT / "tests" / "ci" / "config.json"


def git(args):
    return subprocess.run(["git", "-C", str(REPO_ROOT), *args],
                          capture_output=True, text=True)


def zone_cells(cfg, zone):
    return list(cfg.get("path_zones", {}).get(zone, []))


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--base", default="origin/devel")
    parser.add_argument("--profile")
    parser.add_argument("--cap", type=int, default=0)
    parser.add_argument("paths", nargs="*")
    parser.add_argument("-h", "--help", action="store_true")
    args = parser.parse_args()

    if args.help:
        print(__doc__.strip())
        return 0

    cfg = json.loads(REGISTRY.read_text(encoding="utf-8"))

    if args.profile:
        profiles = cfg.get("profiles", {})
        if args.profile not in profiles:
            print(f"select-cells: unknown profile '{args.profile}'", file=sys.stderr)
            return 1
        for cid in profiles[args.profile]:
            print(cid)
        return 0

    paths = list(args.paths)
    if not paths:
        # git diff --name-only vs base...HEAD + working tree changes + untracked
        base = args.base
        r = git(["rev-parse", "--verify", "-q", base])
        if r.returncode != 0:
            print(f"select-cells: base '{base}' not found (give --base or paths)",
                  file=sys.stderr)
            return 1
        paths = git(["diff", "--name-only", f"{base}...HEAD"]).stdout.splitlines()
        paths += git(["diff", "--name-only"]).stdout.splitlines()
        paths += git(["ls-files", "--others", "--exclude-standard"]).stdout.splitlines()
        paths = [p for p in paths if p]
        if not paths:
            print(f"select-cells: no changes vs {base}", file=sys.stderr)
            return 1

    result = []
    seen = set()
    zones = list(cfg.get("path_zones", {}).keys())
    for p in paths:
        matched = False
        for zone in zones:
            if p.startswith(zone):
                matched = True
                for cid in zone_cells(cfg, zone):
                    if cid not in seen:
                        seen.add(cid)
                        result.append(cid)
        if not matched:
            print(f"select-cells: no zone for '{p}'", file=sys.stderr)

    if not result:
        print("select-cells: no cells selected (docs-only change?)", file=sys.stderr)
        return 1

    # --cap N: shrink keeping platform diversity and push-quick priority.
    if args.cap > 0 and len(result) > args.cap:
        pushquick = set(cfg.get("profiles", {}).get("push-quick", []))
        runs_on = {c["id"]: c.get("runs-on", "") for c in cfg["cells"]}
        capped = []
        chosen = set()
        # 1st pass: intersection with push-quick (highest-value cells).
        for cid in result:
            if len(capped) >= args.cap:
                break
            if cid in pushquick and cid not in chosen:
                chosen.add(cid)
                capped.append(cid)
        # 2nd pass: one cell per runs-on for diversity.
        seen_runson = set()
        for cid in result:
            if len(capped) >= args.cap:
                break
            ro = runs_on.get(cid, "")
            if cid in chosen or not ro or ro in seen_runson:
                continue
            seen_runson.add(ro)
            chosen.add(cid)
            capped.append(cid)
        # 3rd pass: fill the rest in original order.
        for cid in result:
            if len(capped) >= args.cap:
                break
            if cid not in chosen:
                chosen.add(cid)
                capped.append(cid)
        result = capped
        print(f"select-cells: capped {len(result)} cells (--cap {args.cap})",
              file=sys.stderr)

    for cid in result:
        print(cid)
    print(f"select-cells: selected {len(result)} cells for {len(paths)} changed paths",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())