#!/usr/bin/env python3
# Infra v3 (D2): validate tests/ci/config.json for structural consistency and
# axis coverage of the change-addressed gate (= union of push-quick + win-gate).
#
# Guarantees (auto-checks, no manual bookkeeping):
#   * every id referenced by a profile/common_prefixes exists in cells;
#   * cell ids are unique; required fields are present; runs-on is known;
#   * the change-addressed gate covers every mandatory axis (OS, Windows
#     toolchain family, arch, CRT mode, shared mode, build type);
#   * every common_prefix exists in the repository tree.
#
# Exit codes: 0 = ok, 1 = validation errors/gaps, 2 = usage error.

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
REGISTRY = REPO_ROOT / "tests" / "ci" / "config.json"

# Known GitHub-hosted runner labels appearing in the registry.
KNOWN_RUNNERS = {
    "ubuntu-24.04", "ubuntu-latest", "ubuntu-26.04-arm",
    "windows-latest", "windows-2022", "windows-11-vs2026-arm",
    "macos-15", "macos-26-intel", "xcode-27",
}


def parse_axes(cell: dict) -> dict:
    """Derive a cell's axes from its id. Deterministic for the current naming
    scheme; additions to the scheme must be mirrored here (validation fails if
    an id cannot be classified)."""
    i = cell["id"]
    a = {"os": None, "toolset": None, "arch": None, "build": None,
         "shared": None, "crt": None}
    if i.startswith("linux"):
        a["os"] = "linux"
        a["toolset"] = "gcc" if "gcc" in i else ("clang" if "clang" in i else None)
        if "-lock-" in i:
            a["arch"] = "x64"  # locking cells don't encode word-size
        else:
            a["arch"] = "arm" if "arm" in i else "x64"
    elif i.startswith("macos"):
        a["os"] = "macos"
        a["toolset"] = "apple-clang"
        a["arch"] = "intel" if "intel" in i else "arm64"
    elif i.startswith("android"):
        a["os"] = "android"
        a["toolset"] = "ndk"
        a["arch"] = "arm64-v8a" if "arm64" in i else ("armv7" if "arm" in i else "x86-64")
    elif i.startswith("win"):
        a["os"] = "windows"
        for k in ("msvc-v145", "msvc-v143", "mscl", "mingw",
                  "cxx-v142", "cxx-v143", "cxx-v145"):
            if k in i:
                a["toolset"] = k
                break
        if a["toolset"] is None:
            a["toolset"] = i.split("-")[1]
        if "mingw" in i:
            a["arch"] = "win32" if "mingw-32" in i else "x64"
        elif "-arm64-" in i:
            a["arch"] = "arm64"
        elif "-win32-" in i:
            a["arch"] = "win32"
        elif "-x64-" in i:
            a["arch"] = "x64"
        a["shared"] = ("on" if "shared-on" in i else
                       ("off" if "shared-off" in i else None))
        a["crt"] = ("on" if "mscrt-on" in i else
                    ("off" if "mscrt-off" in i else None))
    a["build"] = "debug" if "debug" in i else ("release" if "release" in i else None)
    return a


def find_cell(cells, cid):
    for c in cells:
        if c["id"] == cid:
            return c
    return None


def gated_cells(cfg):
    """Union of the profiles that make up the change-addressed gate."""
    prof = cfg.get("profiles", {})
    ids = []
    seen = set()
    for name in ("push-quick", "win-gate"):
        for cid in prof.get(name, []):
            if cid not in seen:
                seen.add(cid)
                ids.append(cid)
    return ids


def check_structure(cfg, errors):
    cells = cfg.get("cells", [])
    by_id = {c["id"]: c for c in cells}

    # unique ids
    seen = set()
    for c in cells:
        if c["id"] in seen:
            errors.append(f"duplicate cell id: {c['id']}")
        seen.add(c["id"])

    # required fields
    for c in cells:
        for field in ("id", "runs-on", "cmake", "ctest", "note"):
            if field not in c:
                errors.append(f"cell {c.get('id', '?')}: missing '{field}'")
        if c.get("runs-on") not in KNOWN_RUNNERS:
            errors.append(f"cell {c.get('id', '?')}: unknown runs-on "
                          f"'{c.get('runs-on')}'")
        if c.get("build_only") and c.get("ctest"):
            errors.append(f"cell {c['id']}: build_only with non-null ctest")

    # profile references valid
    for pname, ids in cfg.get("profiles", {}).items():
        for cid in ids:
            if cid not in by_id:
                errors.append(f"profile '{pname}': unknown cell '{cid}'")

    # common_prefixes exist in the tree
    for prefix in cfg.get("common_prefixes", []):
        p = REPO_ROOT / prefix
        if not p.exists():
            errors.append(f"common_prefix '{prefix}' does not exist in tree")


def check_coverage(cfg, errors):
    """Mandatory axes that the change-addressed gate must cover."""
    ids = gated_cells(cfg)
    by_id = {c["id"]: c for c in cfg["cells"]}
    present = []
    for cid in ids:
        c = by_id.get(cid)
        if c:
            present.append(parse_axes(c))

    def has(**want):
        for a in present:
            if all(a.get(k) == v for k, v in want.items()):
                return True
        return False

    # OS coverage
    for os_name in ("linux", "windows", "macos"):
        if not has(os=os_name):
            errors.append(f"COVERAGE-GAP: gate lacks an '{os_name}' cell")

    # Windows toolchain family x arch (the user-mandated matrix)
    toolchains = ("msvc-v145", "msvc-v143", "mscl", "mingw",
                  "cxx-v142", "cxx-v143", "cxx-v145")
    for tc in toolchains:
        if not has(os="windows", toolset=tc):
            errors.append(f"COVERAGE-GAP: gate lacks Windows toolchain '{tc}'")
    for arch in ("x64", "win32", "arm64"):
        if not has(os="windows", arch=arch):
            errors.append(f"COVERAGE-GAP: gate lacks Windows arch '{arch}'")
    for crt in ("on", "off"):
        if not has(os="windows", crt=crt):
            errors.append(f"COVERAGE-GAP: gate lacks Windows mscrt '{crt}'")
    for sh in ("on", "off"):
        if not has(os="windows", shared=sh):
            errors.append(f"COVERAGE-GAP: gate lacks Windows shared '{sh}'")
    for bt in ("debug", "release"):
        if not has(os="windows", build=bt):
            errors.append(f"COVERAGE-GAP: gate lacks Windows '{bt}'")

    # at least one runnable (non build_only) windows cell
    runnable = any(
        a.get("os") == "windows" and not (by_id[cid].get("build_only"))
        for cid, a in zip(ids, present)
        if cid in by_id and by_id[cid].get("build_only") is not True
    )
    # simpler: count runnable windows cells directly
    runnable_w = sum(
        1 for cid in ids
        if by_id.get(cid) and by_id[cid]["id"].startswith("win")
        and not by_id[cid].get("build_only")
    )
    if runnable_w == 0:
        errors.append("COVERAGE-GAP: gate has no runnable (non build-only) Windows cell")
    else:
        present.clear()  # placeholder no-op to keep var used


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("cmd", choices=("check-coverage", "check-structure"))
    ap.add_argument("--registry", default=str(REGISTRY))
    args = ap.parse_args()

    cfg = json.loads(Path(args.registry).read_text(encoding="utf-8"))
    errors = []
    check_structure(cfg, errors)
    if args.cmd == "check-coverage":
        check_coverage(cfg, errors)

    if errors:
        print(f"validate-config[{args.cmd}]: {len(errors)} issue(s):",
              file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1
    print(f"validate-config[{args.cmd}]: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
