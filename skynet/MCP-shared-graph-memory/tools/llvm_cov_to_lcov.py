#!/usr/bin/env python3
"""llvm_cov_to_lcov.py — конвертация llvm-cov export (JSON) в lcov lines.info.

Вход: вывод `llvm-cov export -format=text` (JSON: data[].files[].segments,
сегменты [line, col, count, hasCount, isRegionEntry, isGapRegion]).
Выход: lcov lines.info (SF/DA/end_of_record) — совместимый с
tools/lcov_to_coverage.py и gcov-пайплайном shared-graph-memory.

Строка считается покрытой (DA:<line>,1), если в ней начинается или
заканчивается регион со счётчиком >0, либо она лежит в регионе с count>0
(сегмент с hasCount=True и count>0 действует до следующего сегмента).
Строки без счётчиков не попадают в вывод (как в lcov: только исполненные).

Usage:
  python3 tools/llvm_cov_to_lcov.py --json llvm.json --root <repo> > lines.info
"""

import argparse
import json
import os


def covered_lines_from_segments(segments) -> set:
    """Построчное покрытие из llvm-cov segments.

    Сегмент: [line, col, count, hasCount, isRegionEntry, isGapRegion].
    Регион со счётчиком count покрывает строки от своего начала до начала
    следующего сегмента (включительно по правилам llvm-cov: строка сегмента
    тоже принадлежит региону).
    """
    covered = set()
    cur_count = 0
    cur_line = None
    for seg in sorted(segments, key=lambda s: (s[0], s[1])):
        line, _col, count = seg[0], seg[1], seg[2]
        has_count = bool(seg[3]) if len(seg) > 3 else True
        if cur_line is not None and cur_count > 0:
            for ln in range(cur_line, line):
                covered.add(ln)
        if has_count and count > 0:
            covered.add(line)
        cur_count = count if has_count else cur_count
        cur_line = line
    if cur_line is not None and cur_count > 0:
        covered.add(cur_line)
    return covered


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", required=True, help="llvm-cov export JSON")
    ap.add_argument("--root", default="", help="срезать префикс пути (repo root)")
    args = ap.parse_args()

    # Windows runners default to a legacy ANSI codepage; llvm-cov JSON is
    # UTF-8 (source paths etc.), so decode explicitly or cp1252 chokes.
    with open(args.json, encoding="utf-8") as fh:
        doc = json.load(fh)

    out = []
    for data in doc.get("data", []):
        for f in data.get("files", []):
            filename = f.get("filename", "")
            if args.root and filename.startswith(args.root):
                filename = filename[len(args.root):].lstrip("/\\")
            filename = filename.replace("\\\\", "/").replace("\\", "/")
            lines = covered_lines_from_segments(f.get("segments", []))
            if not lines:
                continue
            out.append("SF:%s" % filename)
            for ln in sorted(lines):
                out.append("DA:%d,1" % ln)
            out.append("end_of_record")
    print("\n".join(out))


if __name__ == "__main__":
    main()