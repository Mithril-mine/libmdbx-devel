#!/usr/bin/env python3
"""scan_uncovered.py — «острова смысла»: код вне union-AST всех конфигов.

Находит строки кода, не представленные ни в одном AST-скане (ни в одной
конфигурации). Такие острова — это ветки #if, неактивные во всех
просканированных конфигурациях (например apple/bsd/solaris без SDK,
выключенные опции), либо код, который сканер не смог покрыть.

Для каждого острова фиксирует:
  - file / l0 / l1 — диапазон строк;
  - region — id покрывающего #if-региона (если есть);
  - cls / cond — класс условия региона и само условие (platform/option/...);
  - lines — первые строки острова для ручного разбора.

Метрика: `coverage_pct` — доля «кодовых» строк (без комментариев/пустых),
покрытых хотя бы одним AST-узлом, по всем конфигурациям.

Выход: дописывает раздел `uncovered` + `coverage_pct` в counts артефакта.
"""

import argparse
import bisect
import json
import os
import re
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
REPO_ROOT = os.path.dirname(os.path.dirname(MODULE_DIR))

try:
    from scan_regions import collect_source_files
except ImportError:
    from tools.scan_regions import collect_source_files


_COMMENT_OR_PP = re.compile(
    r'^\s*(#|//|/\*|\*|/\*.*\*/\s*$|$)')


def _code_like(line: str) -> bool:
    """Строка «кодовая»: не пустая, не комментарий, не препроцессор."""
    s = line.strip()
    if not s:
        return False
    if s.startswith("#"):
        return False
    if s.startswith("//"):
        return False
    if s.startswith("/*") or s.startswith("*"):
        return False
    return True


def covered_ranges(symbols: dict) -> dict:
    """file -> список (l0, l1) диапазонов, покрытых AST-узлами.

    Учитывает функции (включая все implementations), типы и макросы.
    Блоки внутри функций не добавляются отдельно — их диапазон всегда
    внутри тела функции (l0..l1 символа).
    """
    out = {}
    for key, sym in symbols.items():
        if sym.get("implementations"):
            for impl in sym["implementations"]:
                f, l0, l1 = impl.get("file"), impl.get("l0", 0), impl.get("l1", 0)
                if f and l0 and l1 >= l0:
                    out.setdefault(f, []).append((l0, l1))
            continue
        f, l0, l1 = sym.get("file"), sym.get("l0", 0), sym.get("l1", 0)
        if not l0 or l1 < l0:
            continue
        out.setdefault(f, []).append((l0, l1))
    for flist in out.values():
        flist.sort()
    return out


def _merge_spans(spans: list) -> list:
    """Схлопывает пересекающиеся/смежные диапазоны (l0, l1)."""
    if not spans:
        return []
    merged = [list(spans[0])]
    for a, b in spans[1:]:
        if a <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], b)
        else:
            merged.append([a, b])
    return [(a, b) for a, b in merged]


def find_islands(filepath: str, covered: list) -> list:
    """Непрерывные «кодовые» строки вне покрытых диапазонов.

    Возвращает список (l0, l1) островов. Покрытые диапазоны — уже
    схлопнутые отсортированные интервалы.
    """
    with open(filepath, errors="replace") as f:
        lines = f.read().splitlines()
    covered = _merge_spans(covered)

    starts = [c[0] for c in covered]
    code_lines = [ln + 1 for ln, line in enumerate(lines) if _code_like(line)]

    islands = []
    for ln in code_lines:
        i = bisect.bisect_right(starts, ln) - 1
        inside = i >= 0 and covered[i][1] >= ln
        if inside:
            continue
        if islands and islands[-1][1] + 1 == ln:
            islands[-1][1] = ln
        else:
            islands.append([ln, ln])
    return [(a, b) for a, b in islands]


def _island_text(filepath: str, island: dict, n: int = 2) -> list:
    with open(filepath, errors="replace") as f:
        lines = f.read().splitlines()
    return [lines[i - 1].strip()[:100] for i in range(island["l0"],
                                                     min(island["l1"], island["l0"] + n) + 1)]


def compute(symbols: dict, regions: list,
            include_repo_dirs=("src",)) -> dict:
    """Считает uncovered-острова и coverage_pct по артефакту."""
    covered = covered_ranges(symbols)
    islands_all = []
    total_code = 0
    covered_code = 0
    for path in collect_source_files():
        rel = os.path.relpath(path, REPO_ROOT)
        if not rel.startswith(include_repo_dirs):
            continue
        spans = _merge_spans(covered.get(rel, []))
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
        n_code = sum(1 for line in lines if _code_like(line))
        n_cov = sum(1 for ln, line in enumerate(lines, 1)
                    if _code_like(line) and _line_covered(ln, spans))
        total_code += n_code
        covered_code += n_cov
        islands = find_islands(path, spans)
        for l0, l1 in islands:
            e = {"file": rel, "l0": l0, "l1": l1}
            e["lines"] = _island_text(path, e)
            islands_all.append(e)

    islands_all.sort(key=lambda e: (e["file"], e["l0"]))
    # классификация после полного списка
    islands_all = _classify_all(islands_all, regions)
    pct = round(100.0 * covered_code / total_code, 2) if total_code else 100.0
    return {"islands": islands_all, "covered_code_lines": covered_code,
            "total_code_lines": total_code, "coverage_pct": pct}


def _line_covered(ln: int, spans: list) -> bool:
    starts = [s[0] for s in spans]
    i = bisect.bisect_right(starts, ln) - 1
    return i >= 0 and spans[i][1] >= ln


def _classify_all(islands: list, regions: list) -> list:
    by_file = {}
    for r in regions:
        by_file.setdefault(r["file"], []).append(r)
    for flist in by_file.values():
        flist.sort(key=lambda r: (r["l0"], -(r.get("l1") or 0)))
    out = []
    for e in islands:
        flist = by_file.get(e["file"], [])
        starts = [r["l0"] for r in flist]
        i = bisect.bisect_right(starts, e["l0"]) - 1
        best = None
        while i >= 0:
            r = flist[i]
            rl1 = r.get("l1") or e["l1"]
            if r["l0"] <= e["l0"] and rl1 >= e["l1"]:
                # самый внутренний покрывающий регион = минимальный диапазон
                if best is None or (rl1 - r["l0"]) < (best["l1"] - best["l0"]):
                    best = r
            i -= 1
        if best:
            e["region"] = best["id"]
            e["cls"] = best.get("cls", "unknown")
            e["cond"] = best.get("cond", "")
        out.append(e)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default=os.path.join(TOOLS_DIR, "artifacts",
                                                  "refactoring-map.json"))
    ap.add_argument("--out", default=None, help="куда писать (default: тот же --map)")
    ap.add_argument("--summary", action="store_true",
                    help="только сводка по классам островов")
    args = ap.parse_args()

    with open(args.map) as f:
        artifact = json.load(f)
    res = compute(artifact["symbols"], artifact["regions"])
    out_path = args.out or args.map
    artifact["uncovered"] = res["islands"]
    artifact["counts"]["coverage_pct"] = res["coverage_pct"]
    artifact["counts"]["uncovered_lines"] = sum(
        i["l1"] - i["l0"] + 1 for i in res["islands"])

    if args.summary:
        from collections import Counter
        cnt = Counter(i.get("cls", "none") for i in res["islands"])
        print("coverage_pct: %.2f%%  (%d/%d)" % (
            res["coverage_pct"], res["covered_code_lines"],
            res["total_code_lines"]))
        print("islands:", len(res["islands"]),
              "по классам:", dict(cnt))
        return 0

    with open(out_path, "w") as f:
        json.dump(artifact, f, ensure_ascii=False, indent=1)
    print("uncovered: %d islands, coverage_pct=%.2f%% -> %s" % (
        len(res["islands"]), res["coverage_pct"], out_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())