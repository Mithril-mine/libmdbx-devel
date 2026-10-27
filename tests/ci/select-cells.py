#!/usr/bin/env python3
# Infra v3 (C1): diff -> cell selection (CI v3.5 model, no path_zones).
#
# Maps the changed paths onto a two-tier policy:
#   * any change touching a "common prefix" (src/, cmake/, CMakeLists.txt,
#     mdbx.h, mdbx.h++, mdbx++/, GNUmakefile, Makefile, tests/ut/,
#     tests/framework/, tests/ci/, .github/workflows/) -> the change-addressed
#     gate = union(push-quick, win-gate);
#   * otherwise (docs/, skynet/, isolated files) -> push-quick.
# A gate covers the full Windows axis matrix (see validate-config.py), so a
# breakage in common code is always seen on every platform/toolset.
#
# Usage:
#   tests/ci/select-cells.py [--base <ref>] [path...]
#   tests/ci/select-cells.py                    # changed = git diff vs base
#   tests/ci/select-cells.py src/cursor.c        # explicit paths
#   tests/ci/select-cells.py --profile NAME      # fixed profile (gate/others)
#   tests/ci/select-cells.py --cap N             # shrink to N diverse cells
#
# Output: newline-separated cell ids on stdout; stats on stderr.
# Exit: 0 = ok (non-empty selection ensured), 1 = nothing selected,
#       2 = usage/error.

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


def profile_cells(cfg, name):
    prof = cfg.get("profiles", {})
    if name not in prof:
        print(f"select-cells: unknown profile '{name}'", file=sys.stderr)
        return None
    return list(prof[name])


def change_addressed_cells(cfg):
    """union(push-quick, win-gate), deduplicated, order-preserving."""
    seen = set()
    out = []
    for name in ("push-quick", "win-gate"):
        for cid in cfg.get("profiles", {}).get(name, []):
            if cid not in seen:
                seen.add(cid)
                out.append(cid)
    return out


def hits_common(cfg, paths):
    prefixes = cfg.get("common_prefixes", [])
    for p in paths:
        for pref in prefixes:
            if p == pref or p.startswith(pref):
                return True
    return False


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
        if args.profile == "change-addressed":
            result = change_addressed_cells(cfg)
        else:
            result = profile_cells(cfg, args.profile)
        if result is None:
            return 1
    else:
        paths = list(args.paths)
        if not paths:
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
        # two-tier policy: common -> change-addressed gate, else push-quick
        if hits_common(cfg, paths):
            result = change_addressed_cells(cfg)
            print("select-cells: common change -> change-addressed gate "
                  f"({len(result)} cells)", file=sys.stderr)
        else:
            result = profile_cells(cfg, "push-quick") or []
            print("select-cells: non-common change -> push-quick "
                  f"({len(result)} cells)", file=sys.stderr)

    if not result:
        print("select-cells: nothing selected (gate may be empty)", file=sys.stderr)
        return 1

    # --cap N: shrink keeping platform diversity and gate priority.
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
    print(f"select-cells: selected {len(result)} cells", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
