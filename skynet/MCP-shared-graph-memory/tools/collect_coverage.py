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
                    continue  # некорректная DA:-запись — пропускаем строку
    return files


def parse_llvm_cov_json(path: str) -> dict:
    """llvm-cov export -format=json → {abs_file: set(covered_line)}.

    Сегменты — границы состояния покрытия [line, col, count, hasCount,
    isGapRegion]: сегмент с count>0 покрывает строки ОТ своей позиции ДО
    следующей границы (half-open interval). GAP-регионы (isGapRegion=true)
    НЕ означают исполнение — они наследуют счётчик без кода, поэтому строки
    гэпа не добавляются в covered (иначе фальшивое покрытие).
    """
    files = {}
    with open(path) as fh:
        data = json.load(fh)
    for f in data.get("data", []):
        for fld in f.get("files", []):
            fname = fld.get("filename")
            seg = fld.get("segments", [])
            covered = set()
            for i, seg_ in enumerate(seg):
                if len(seg_) < 3 or seg_[2] <= 0:
                    continue
                if len(seg_) >= 5 and seg_[4]:
                    continue  # gap-регион: счётчик унаследован, кода нет
                start_line = seg_[0]
                if i + 1 < len(seg) and len(seg[i + 1]) >= 2:
                    next_line = seg[i + 1][0]
                    if next_line > start_line:
                        # half-open интервал строк: [start, next_line) —
                        # строка next_line принадлежит следующему сегменту
                        end_line = next_line - 1
                    else:
                        # та же строка: граница по колонке, строка покрыта
                        end_line = start_line
                else:
                    end_line = start_line
                for ln in range(start_line, end_line + 1):
                    covered.add(ln)
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
    # индексы: абс-путь; basename → покрытые строки, но ТОЛЬКО если имя
    # однозначно и в покрытии, и в КАРТЕ (иначе b/foo.c ложно возьмёт a/foo.c)
    idx_abs = {os.path.abspath(p): s for p, s in files_covered.items()}
    # distinct-файлы карты, включая implementations (не число символов!)
    map_files = set()
    for ms in symbol_map.values():
        if ms.get("file"):
            map_files.add(ms["file"])
        for impl in ms.get("implementations") or []:
            if impl.get("file"):
                map_files.add(impl["file"])
    base_file_count = {}
    for f in map_files:
        b = os.path.basename(f)
        base_file_count[b] = base_file_count.get(b, 0) + 1
    idx_base = {}
    for p, s in files_covered.items():
        b = os.path.basename(p)
        # имя встречается в покрытии или карте неоднозначно — fallback опасен
        if sum(1 for q in files_covered if os.path.basename(q) == b) > 1:
            continue
        if base_file_count.get(b, 0) > 1:
            continue
        idx_base[b] = s
    hits = []
    blocks = []
    for sym, info in symbol_map.items():
        # для merged-карт тела лежат в implementations (по конфигурациям):
        # собираем файлы/диапазоны и из них, а не только из верхнего card
        impls = info.get("implementations") or []
        regions = [(info.get("file"), info.get("l0"), info.get("l1"))]
        if impls:
            regions = [(i.get("file"), i.get("l0"), i.get("l1")) for i in impls]
            if not any(all(v is not None for v in r) for r in regions):
                continue
        covered_any = False
        for f, l0, l1 in regions:
            if f is None or l0 is None or l1 is None:
                continue
            lines = None
            if f in files_covered:
                lines = files_covered[f]  # точное совпадение (даже пустое)
            elif os.path.abspath(f) in idx_abs:
                lines = idx_abs[os.path.abspath(f)]
            elif os.path.basename(f) in idx_base:
                lines = idx_base[os.path.basename(f)]
            if lines is None:
                continue
            if any(x in lines for x in range(l0, l1 + 1)):
                covered_any = True
                break
        if covered_any:
            hits.append(sym)
        # блочная детализация по телам implementations
        for f, bl0, bl1, bl_id in _block_records(info):
            lines = None
            if f in files_covered:
                lines = files_covered[f]
            elif os.path.abspath(f) in idx_abs:
                lines = idx_abs[os.path.abspath(f)]
            elif os.path.basename(f) in idx_base:
                lines = idx_base[os.path.basename(f)]
            if lines is None:
                continue
            covered = any(x in lines for x in range(bl0, bl1 + 1))
            blocks.append({
                "symbol": sym, "block_id": bl_id,
                "l0": bl0, "l1": bl1, "covered": covered,
            })
    return hits, blocks


def _block_records(info) -> list:
    """[(file, l0, l1, block_id)] — блоки из карты и её implementations.

    Блоки разных реализаций (config) сохраняют свой файл; итоговый
    block_id уникален на файл+конфигурацию (config добавляется к id),
    чтобы два тела с одинаковыми id не сливались в symbol_hits.
    """
    out = []
    for b in info.get("blocks") or []:
        out.append((info.get("file"), b.get("l0"), b.get("l1"),
                    b.get("id", "")))
    for impl in info.get("implementations") or []:
        cfg = impl.get("config", "")
        for b in impl.get("blocks") or []:
            bid = b.get("id", "")
            if cfg:
                bid = "%s@%s" % (bid, cfg)
            out.append((impl.get("file"), b.get("l0"), b.get("l1"), bid))
    return out


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
        # ЕДИНЫЙ слой для обоих писателей: явный --layer или активная ветка
        res = store.coverage_insert(args.test_id, sorted(hits),
                                    pct=args.pct or 0.0,
                                    layer=args.layer)
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
