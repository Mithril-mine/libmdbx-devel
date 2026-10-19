#!/usr/bin/env python3
"""gcov_to_coverage.py — загрузка реального покрытия (gcov .gcda) в Store.

Запускает `gcov -b` по всем .gcda каталога, парсит .gcov-файлы
(строки `N:` подсчёт>0 = покрыто; `#####:` = непокрыто), собирает
{file → covered_lines}, затем через карту (refactoring-map) резолвит
символы и БЛОКИ и пишет в coverage/coverage_blocks (как один «тест»).

Использование:
  python3 tools/gcov_to_coverage.py --gcda-dir build/CMakeFiles/mdbx-static.dir/src \\
      --map tools/artifacts/refactoring-map-linux.json \\
      --test-id smoke_basic --layer 0.15 --db <path> [--store-blocks]
"""

import argparse
import glob
import json
import os
import re
import subprocess
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, MODULE_DIR)

from mcp import Store  # noqa: E402
from tools.collect_coverage import symbol_hits  # noqa: E402

_COVERED_RE = re.compile(r"^\s*(\d+):\s*(\d+):")


def parse_gcov(path: str) -> tuple:
    """(source_file, covered_lines) из .gcov."""
    src = None
    covered = set()
    with open(path, errors="replace") as fh:
        for line in fh:
            m = re.match(r"^\s*-:\s*0:Source:(.+)$", line)
            if m:
                src = m.group(1).strip()
                continue
            m = _COVERED_RE.match(line)
            if m:
                covered.add(int(m.group(2)))
    return src, covered


def run_gcov(gcda_dir: str) -> dict:
    """Запускает gcov на каждом .gcda, собирает {abs_path: covered_lines}."""
    files = {}
    for gcda in sorted(glob.glob(os.path.join(gcda_dir, "**", "*.gcda"),
                                 recursive=True)):
        # gcov создаёт .gcov рядом (в CWD); запускаем в gcda_dir
        subprocess.run(["gcov", "-b", gcda],
                       cwd=os.path.dirname(gcda),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        base_name = os.path.basename(gcda)[:-len(".gcda")]
        cand = os.path.join(os.path.dirname(gcda), base_name + ".gcov")
        if os.path.exists(cand):
            src, covered = parse_gcov(cand)
            if src and covered:
                files.setdefault(src, set()).update(covered)
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gcda-dir", required=True)
    ap.add_argument("--map", default=os.path.join(TOOLS_DIR, "artifacts",
                                                  "refactoring-map-linux.json"))
    ap.add_argument("--test-id", required=True)
    ap.add_argument("--layer", default="")
    ap.add_argument("--db", default=os.path.join(
        os.path.expanduser("~"), ".local", "share", "shared-graph-memory",
        "db.mdbx"))
    ap.add_argument("--store-blocks", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    t0 = time.time()
    files = run_gcov(args.gcda_dir)
    print("gcov: %d files with covered lines in %.1fs" % (len(files),
                                                          time.time() - t0))

    if not os.path.exists(args.map):
        print("map not found: %s" % args.map, file=sys.stderr)
        return 2
    with open(args.map) as fh:
        symbols = json.load(fh).get("symbols", {})
    hits, blocks = symbol_hits(files, symbols)
    print("coverage: %d symbols hit, %d block-records" % (len(hits),
                                                          len(blocks)))
    if args.dry_run:
        for s in sorted(hits)[:30]:
            print("  ", s)
        return 0

    with Store(args.db) as store:
        pre = store.layer_prefix(args.layer)
        r = store.coverage_insert((pre + args.test_id), sorted(hits))
        print("coverage_insert: %s -> %d symbols" % (r["test"], r["symbols"]))
        if args.store_blocks and blocks:
            n = store.coverage_blocks_insert(args.test_id, blocks,
                                             layer=args.layer)
            print("coverage_blocks: %d block-records" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())