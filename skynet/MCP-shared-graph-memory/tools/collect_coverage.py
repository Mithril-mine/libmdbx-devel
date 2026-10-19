#!/usr/bin/env python3
"""collect_coverage.py — загрузка покрытия тестов (lcov/gcov) в Store.

Резолвит покрытые строки из lcov-данных (`lcov` / `llvm-cov export` /
`gcov -i`) в набор символов карты исходников (refactoring-map.json) по
диапазонам l0/l1 и пишет в `coverage` (test_id → symbols) + обратный
индекс `covered_by` (символ → тесты). Это делает слой «тест → код»
первоклассным: по символу можно найти защищающие его тесты.

Источники:
  --lcov <file>            lcov-файл (SF:/DA:/LH:/LF:) из одного прогона
  --llvm-cov-json <file>   вывод `llvm-cov export -format=json`
  --gcov-dir <dir>         каталог .gcda/.gcno от gcov (через lcov --capture)
  --map <refactoring-map.json>   карта символов (по умолчанию tools/artifacts/)

Карта символов задаёт соответствие file -> [(l0,l1,key)]. Для каждого
покрытого файла берём набор строк; символ покрыт, если его [l0,l1]
пересекается с покрытым диапазоном.

Использование:
  python3 tools/collect_coverage.py --lcov build/coverage/ut.info \\
      --map tools/artifacts/refactoring-map.json \\
      --test-id ut --db ~/.local/share/shared-graph-memory/db.mdbx
"""

import argparse
import json
import os
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, MODULE_DIR)

from mcp import Store  # noqa: E402

DEFAULT_MAP = os.path.join(TOOLS_DIR, "artifacts", "refactoring-map.json")


def parse_lcov(path: str) -> dict:
    """lcov → {abs_file: set(covered_line)}."""
    files = {}
    cur = None
    with open(path, errors="replace") as fh:
        for line in fh:
            line = line.rstrip()
            if line.startswith("SF:"):
                cur = line[3:]
                files.setdefault(cur, set())
            elif line.startswith("DA:") and cur is not None:
                # DA:<line>,<count>[,<checksum>]
                ln = line[3:].split(",", 1)[0]
                try:
                    if int(line.split(",")[1]) > 0:
                        files[cur].add(int(ln))
                except (ValueError, IndexError):
                    # некорректная DA:-запись (мусор/хвост файла): пропускаем
                    pass
    return files


def parse_llvm_cov_json(path: str) -> dict:
    """llvm-cov export -format=json → {abs_file: set(covered_line)}."""
    files = {}
    with open(path) as fh:
        data = json.load(fh)
    for f in data.get("data", []):
        for fld in f.get("files", []):
            fname = fld.get("filename")
            seg = fld.get("segments", [])
            # segments: [line, col, count, ...]; покрытие строки где count>0
            covered = set()
            for seg_ in seg:
                if len(seg_) >= 3 and seg_[2] > 0:
                    covered.add(seg_[0])
            if fname:
                files.setdefault(fname, set()).update(covered)
    return files


def symbol_hits(files_covered, symbol_map) -> tuple:
    """Символы и блоки карты, пересекающиеся с покрытыми строками.

    Возвращает (symbols, blocks):
      symbols — набор fq-ключей, где хотя бы одна строка покрыта;
      blocks  — [{"symbol", "block_id", "l0", "l1", "covered"}] для каждого
                блока символа (покрыт, если любая его строка в covered).
    """
    # индексы: абс-путь и basename → покрытые строки (gcov даёт абс; карта — относит.)
    idx_abs = {os.path.abspath(p): s for p, s in files_covered.items()}
    idx_base = {}
    for p, s in files_covered.items():
        idx_base.setdefault(os.path.basename(p), set()).update(s)
    hits = []
    blocks = []
    for sym, info in symbol_map.items():
        f = info.get("file")
        if not f:
            continue
        lines = (files_covered.get(f)
                 or idx_abs.get(os.path.abspath(f))
                 or idx_base.get(os.path.basename(f)))
        if not lines:
            continue
        l0, l1 = info.get("l0", 0), info.get("l1", 0)
        # символ покрыт, если любая его строка входит в покрытое множество
        if any(x in lines for x in range(l0, l1 + 1)):
            hits.append(sym)
        # блочная детализация: каждый блок (id, l0, l1) — покрыт/нет
        for b in info.get("blocks", []) or []:
            bl0, bl1 = b.get("l0", 0), b.get("l1", 0)
            covered = any(x in lines for x in range(bl0, bl1 + 1))
            blocks.append({
                "symbol": sym, "block_id": b.get("id", ""),
                "l0": bl0, "l1": bl1, "covered": covered,
            })
    return hits, blocks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lcov", default=None)
    ap.add_argument("--llvm-cov-json", default=None)
    ap.add_argument("--map", default=DEFAULT_MAP)
    ap.add_argument("--test-id", required=True,
                    help="идентификатор теста/домена (например 'ut', 'ut/api/cursor-api')")
    ap.add_argument("--pct", type=float, default=None,
                    help="доля покрытия строк (LH/LF) если известна")
    ap.add_argument("--db", default=os.path.join(
        os.path.expanduser("~"), ".local", "share", "shared-graph-memory",
        "db.mdbx"))
    ap.add_argument("--layer", default="",
                    help="слой ветки (см. docs/BRANCHES.md); base — пусто")
    ap.add_argument("--store-blocks", action="store_true",
                    help="сохранить блочную детализацию (coverage_blocks)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    if args.lcov:
        files = parse_lcov(args.lcov)
    elif args.llvm_cov_json:
        files = parse_llvm_cov_json(args.llvm_cov_json)
    else:
        ap.error("укажите --lcov или --llvm-cov-json")

    if not os.path.exists(args.map):
        print("map не найден: %s (запустите scan_symbols.py --merge)" % args.map,
              file=sys.stderr)
        return 2
    with open(args.map) as fh:
        artifact = json.load(fh)
    symbols = artifact.get("symbols", {})
    hits, blocks = symbol_hits(files, symbols)
    print("coverage: %d files, %d symbols hit, %d block-records"
          % (len(files), len(hits), len(blocks)))
    if args.dry_run:
        for s in sorted(hits)[:30]:
            print("  ", s)
        return 0

    with Store(args.db) as store:
        t0 = time.time()
        pre = store.layer_prefix(args.layer)
        res = store.coverage_insert((pre + args.test_id), sorted(hits),
                                    pct=args.pct or 0.0)
        dt = time.time() - t0
        print("coverage_insert: %s -> %d symbols in %.1fs"
              % (res.get("test"), res.get("symbols"), dt))
        if args.store_blocks and blocks:
            # блочная детализация: coverage_blocks:{test_id}:{symbol} -> [blocks]
            n = store.coverage_blocks_insert(args.test_id, blocks,
                                             layer=args.layer)
            print("coverage_blocks: %d block-records stored" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())