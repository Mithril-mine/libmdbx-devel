#!/usr/bin/env python3
"""run_coverage.py — пер-тестовое покрытие: каким тестом покрыт каждый блок.

Пайплайн:
  1. сборка с -DENABLE_GCOV=ON (или готовый --build-dir);
  2. ctest -N → список тестов (--filter ограничивает);
  3. для каждого теста: очистить *.gcda → прогнать ОДИН тест →
     gcov_to_coverage.run_gcov → symbol_hits → накопление в буфер;
  4. финал: ОДНА coverage_batch + ОДНА coverage_blocks_batch (A3: 2 write-txn
     на прогон вместо 2×N — writer-лок роя не удерживается между тестами),
     затем Store.coverage_summary (union: блок покрыт ≥1 тестом).

Тесты прогоняются ПОСЛЕДОВАТЕЛЬНО (B1-β): параллельный ctest смешал бы
счётчики gcda между тестами.

Использование:
  python3 tools/run_coverage.py --build-dir ../../../../build-gcov \\
      --db ~/.local/share/shared-graph-memory/db.mdbx [--filter 'ut/'] \\
      [--layer 0.15.base] [--map tools/artifacts/refactoring-map-linux.json]
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
for _p in (MODULE_DIR, TOOLS_DIR):
    if _p not in sys.path:
        sys.path.insert(0, _p)

from mcp import Store  # noqa: E402
from collect_coverage import symbol_hits  # noqa: E402
from gcov_to_coverage import run_gcov  # noqa: E402


def ctest_list(build_dir: str, filt: str = "") -> list:
    """ctest -N → имена тестов (опционально по -R фильтру)."""
    cmd = ["ctest", "--test-dir", build_dir, "-N"]
    if filt:
        cmd += ["-R", filt]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("ctest -N failed: %s" % p.stderr[:300], file=sys.stderr)
        return []
    out = []
    for line in p.stdout.splitlines():
        line = line.strip()
        if line.startswith("Test #"):
            # "Test #12: name ...."
            name = line.split(":", 1)[1].strip()
            out.append(name)
    return out


def clear_gcda(build_dir: str) -> int:
    """Удаляет все *.gcda в build_dir (сброс счётчиков между тестами)."""
    n = 0
    for f in glob.glob(os.path.join(build_dir, "**", "*.gcda"),
                       recursive=True):
        try:
            os.unlink(f)
            n += 1
        except OSError as e:
            print("cannot remove %s: %s" % (f, e), file=sys.stderr)
    return n


def run_one_test(build_dir: str, test_name: str, timeout: int = 600) -> bool:
    """Прогон ОДНОГО ctest-теста. Возвращает успех."""
    cmd = ["ctest", "--test-dir", build_dir, "-R",
           "^%s$" % test_name, "--timeout", str(timeout)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode == 0


def gcda_dirs(build_dir: str) -> list:
    """Каталоги, содержащие *.gcda (для run_gcov)."""
    dirs = set()
    for f in glob.glob(os.path.join(build_dir, "**", "*.gcda"),
                       recursive=True):
        dirs.add(os.path.dirname(f))
    return sorted(dirs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build-dir", required=True,
                    help="каталог сборки с -DENABLE_GCOV=ON")
    ap.add_argument("--filter", default="",
                    help="ctest -R фильтр (напр. 'ut/' или '^cache-get')")
    ap.add_argument("--map", default=os.path.join(
        TOOLS_DIR, "artifacts", "refactoring-map-linux.json"))
    ap.add_argument("--db", default=os.path.join(
        os.path.expanduser("~"), ".local", "share", "shared-graph-memory",
        "db.mdbx"))
    ap.add_argument("--layer", default="",
                    help="слой ветки (пустой — активная/базовый)")
    ap.add_argument("--timeout", type=int, default=600,
                    help="таймаут одного теста, сек")
    ap.add_argument("--dry-run", action="store_true",
                    help="только список тестов, без прогонов")
    args = ap.parse_args()

    build_dir = os.path.abspath(args.build_dir)
    if not os.path.isdir(build_dir):
        print("build-dir не существует: %s" % build_dir, file=sys.stderr)
        return 2

    tests = ctest_list(build_dir, args.filter)
    if not tests:
        print("тесты не найдены (filter=%r)" % args.filter, file=sys.stderr)
        return 2
    print("тестов к прогону: %d" % len(tests))
    if args.dry_run:
        for t in tests:
            print("  ", t)
        return 0

    if not os.path.exists(args.map):
        print("карта не найдена: %s (запустите scan_symbols.py)" % args.map,
              file=sys.stderr)
        return 2
    with open(args.map) as fh:
        symbol_map = json.load(fh).get("symbols", {})

    t0 = time.time()
    ok_tests, failed_tests = [], []
    cov_tests, blk_tests = {}, {}
    with Store(args.db) as store:
        for i, test in enumerate(tests, 1):
            clear_gcda(build_dir)
            ok = run_one_test(build_dir, test, timeout=args.timeout)
            (ok_tests if ok else failed_tests).append(test)
            if not ok:
                print("[%d/%d] %s — FAILED (покрытие всё равно снимаем)"
                      % (i, len(tests), test))
            dirs = gcda_dirs(build_dir)
            if not dirs:
                print("[%d/%d] %s — нет gcda (пропуск)" % (i, len(tests), test))
                continue
            files = {}
            for d in dirs:
                for f, cov in run_gcov(d).items():
                    files.setdefault(f, set()).update(cov)
            hits, blocks = symbol_hits(files, symbol_map)
            # накапливаем, а не пишем поштучно (A3)
            cov_tests[test] = {"symbols": sorted(hits), "pct": 0.0}
            if blocks:
                blk_tests[test] = blocks
            print("[%d/%d] %s — %s, symbols=%d blocks=%d (%.1fs)"
                  % (i, len(tests), test, "ok" if ok else "FAIL",
                     len(hits), len(blocks), time.time() - t0))
        # две write-txn на весь прогон вместо 2×N (A3)
        store.coverage_batch(cov_tests, layer=args.layer)
        if blk_tests:
            store.coverage_blocks_batch(blk_tests, layer=args.layer)
        summary = store.coverage_summary(layer=args.layer)
    print("=== итоги: %d ok, %d failed, %.1fs ==="
          % (len(ok_tests), len(failed_tests), time.time() - t0))
    print("coverage_summary total:", json.dumps(summary.get("total", {})))
    return 0


if __name__ == "__main__":
    sys.exit(main())
