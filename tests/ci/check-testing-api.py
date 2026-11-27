#!/usr/bin/env python3
"""The testing-API regulation lint (tests/AGENTS.md §8, issue #519).

Checks two invariants of the staged "no direct inclusion" campaign:

(a) every `MDBX_TST_*` mirror macro defined in `tests/api.h` is pinned with
    a `STATIC_ASSERT`/`STATIC_ASSERT_MSG` in `tests/api.c` - a drift of a
    mirrored constant is a build break, not a silent test skew;
(b) the direct `#include`s of library sources (`src/...`) within `tests/ut`
    and `tests/framework` stay inside the sanctioned list of issue #519 -
    the list shrinks to empty as the rework stages land, and any new
    direct inclusion outside it fails the lint.

Usage: check-testing-api.py [repo-root]  (defaults to the repo of this file)
Exit codes: 0 = clean, 1 = violations found (printed to stdout).
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.dirname(HERE))

API_H = os.path.join(ROOT, "tests", "api.h")
API_C = os.path.join(ROOT, "tests", "api.c")

# The sanctioned direct inclusions of library sources within the gtest
# units, keyed by the path under tests/ (POSIX slashes). The issue-test and
# API-corpus entries shrink as the #519 stages land; the durability-corpus
# and framework entries are the stage-6 items.
SANCTIONED = {
    # The stage-4 algorithm selftests live in the testing-API family
    # (tests/api/) with sanctioned internals access (tests/AGENTS.md §8).
    "api/details_pnl_selftest.c": {"src/pnl.c"},
    "api/details_rkl_selftest.c": {"src/rkl.c", "src/txl.c"},
    "ut/api/meta_validate_corpus.c": {"src/cogs.c", "src/meta.c"},
    "ut/api/crafted_meta_v023.c": {"src/cogs.c", "src/meta.c"},
    "ut/api/crafted_meta_v039.c": {"src/cogs.c", "src/meta.c"},
    "ut/api/probes.c": {"src/essentials.h"},
    "ut/api/probes_ipc.c": {"src/essentials.h"},
    "ut/durability/crash-storm.c++": {"src/essentials.h"},
    "ut/durability/crash-windows.c++": {"src/essentials.h"},
    "ut/durability/dead-owner.c++": {"src/essentials.h"},
    "ut/durability/lock-events.c++": {"src/essentials.h"},
    "framework/base.h++": {"src/essentials.h", "src/osal/osal.h", "src/options.h"},
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s+"([^"]*)"', re.M)
MIRROR_RE = re.compile(r'^#\s*define\s+(MDBX_TST_[A-Za-z0-9_]+)', re.M)
PIN_RE = re.compile(r'STATIC_ASSERT(?:_MSG)?\s*\([^;]*\b({})\b'.format(r"[A-Za-z0-9_]+"))


def check_mirror_pins():
    """Every MDBX_TST_* macro in tests/api.h has a STATIC_ASSERT in tests/api.c."""
    with open(API_H, encoding="utf-8") as f:
        header = f.read()
    with open(API_C, encoding="utf-8") as f:
        impl = f.read()
    mirrors = MIRROR_RE.findall(header)
    problems = []
    for name in mirrors:
        pattern = re.compile(r'STATIC_ASSERT(?:_MSG)?\s*\([^;]*\b{}\b'.format(name))
        if not pattern.search(impl):
            problems.append("the mirror macro {} has no STATIC_ASSERT pin in tests/api.c".format(name))
    return mirrors, problems


def check_sanctioned_includes():
    """No direct src/-includes in tests/ut and tests/framework beyond the sanctioned list."""
    problems = []
    src_root = os.path.normpath(os.path.join(ROOT, "src"))
    tests_root = os.path.normpath(os.path.join(ROOT, "tests"))
    scan_roots = [os.path.normpath(os.path.join(tests_root, "ut")), os.path.normpath(os.path.join(tests_root, "framework"))]
    for root in scan_roots:
        for dirpath, _, names in os.walk(root):
            for name in names:
                if not name.endswith((".c", ".c++", ".h++", ".h")):
                    continue
                path = os.path.normpath(os.path.join(dirpath, name))
                unit_key = os.path.relpath(path, tests_root).replace(os.sep, "/")
                allowed = SANCTIONED.get(unit_key, set())
                with open(path, encoding="utf-8", errors="replace") as f:
                    text = f.read()
                for m in INCLUDE_RE.finditer(text):
                    target = m.group(1)
                    resolved = os.path.normpath(os.path.join(os.path.dirname(path), target))
                    if not resolved.startswith(src_root + os.sep):
                        continue  # not an include of the library sources
                    src_ref = "src/" + os.path.relpath(resolved, src_root).replace(os.sep, "/")
                    if src_ref not in allowed:
                        problems.append("{}: unsanctioned direct inclusion of {} (issue #519)".format(unit_key, src_ref))
    return problems


def main():
    mirrors, problems = check_mirror_pins()
    problems += check_sanctioned_includes()
    if problems:
        print("testing-API regulation violations (tests/AGENTS.md §8, issue #519):")
        for item in problems:
            print("  - " + item)
        print("mirrors found in tests/api.h: " + (", ".join(mirrors) if mirrors else "(none)"))
        return 1
    print("testing-API regulation: clean ({} mirror(s) pinned, the src/-includes within the sanctioned list)".format(
        len(mirrors)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
