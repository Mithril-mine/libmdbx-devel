#!/usr/bin/env python3
"""lcov_to_coverage.py — загрузка lcov-отчёта (lines.info) в Store.

Читает артефакт coverage-$cell из CI (или локальный lcov): формат
    SF:<source-file>
    DA:<line>,<execution-count>
    ...
    end_of_record
и собирает {abs_path: set(covered_lines)} (строки со счётчиком > 0).
Затем через карту (refactoring-map) и symbol_hits() резолвит символы и
блоки, и пишет как один «тест» (coverage_insert + coverage_blocks_insert,
A3: две write-txn на прогон).

Использование:
  python3 tools/lcov_to_coverage.py --info lines.info \
      --map tools/artifacts/refactoring-map.json \
      --test-id win-mingw-gcov --layer "" --db <path>
"""

import argparse
import json
import os
import re
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, MODULE_DIR)

from mcp import Store  # noqa: E402
from tools.collect_coverage import symbol_hits  # noqa: E402

_SF_RE = re.compile(r"^SF:(.+)$")
_DA_RE = re.compile(r"^DA:(\d+),(\d+)")


def parse_lcov(info_path: str) -> dict:
    """{abs_path: set(covered_lines)}; строки со счётчиком 0 — не покрыты."""
    files = {}
    cur = None
    covered = None
    with open(info_path, errors="replace") as fh:
        for line in fh:
            line = line.rstrip("\n")
            m = _SF_RE.match(line)
            if m:
                cur = m.group(1)
                covered = files.setdefault(cur, set())
                continue
            if line == "end_of_record":
                cur = None
                covered = None
                continue
            if cur is None or covered is None:
                continue
            m = _DA_RE.match(line)
            if m and int(m.group(2)) > 0:
                covered.add(int(m.group(1)))
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--info", required=True, help="путь к lines.info (lcov)")
    ap.add_argument("--map", default=os.path.join(TOOLS_DIR, "artifacts",
                                                  "refactoring-map.json"))
    ap.add_argument("--test-id", required=True)
    ap.add_argument("--layer", default="")
    ap.add_argument("--db", default=os.path.join(
        os.path.expanduser("~"), ".local", "share", "shared-graph-memory",
        "db.mdbx"))
    # Блочная детализация — по умолчанию: это единственный источник честных
    # %-по-блокам в coverage_summary (символьный уровень даёт только
    # да/нет по функциям). --no-store-blocks — для быстрых пробных загрузок.
    ap.add_argument("--store-blocks", dest="store_blocks",
                    action="store_true", default=True)
    ap.add_argument("--no-store-blocks", dest="store_blocks",
                    action="store_false")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    t0 = time.time()
    files = parse_lcov(args.info)
    print("lcov: %d files with covered lines in %.1fs" % (len(files),
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
        r = store.coverage_insert(args.test_id, sorted(hits),
                                  layer=args.layer)
        print("coverage_insert: %s -> %d symbols" % (r["test"], r["symbols"]))
        if args.store_blocks and blocks:
            n = store.coverage_blocks_insert(args.test_id, blocks,
                                             layer=args.layer)
            print("coverage_blocks: %d block-records" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())