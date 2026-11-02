#!/usr/bin/env python3
"""validate_map.py — валидатор генератора карты исходников.

Повторно сканирует фиксированные fixture-файлы и проверяет здравые
инварианты собранной карты. Назначение: поймать регрессии ГЕНЕРАТОРА
(пропали символы/рёбра/блоки, разъехались макросы) — независимо от
объёма и изменений ядра libmdbx.

Принципиально: инварианты считаются ТОЛЬКО по fixture-файлам
(tests/fixtures/scan_fixture.*), которые меняются вместе со сканером и
не зависят от рефакторинга ядра. Абсолютные счётчики ядра (сколько
функций в src/**) — легитимно меняются при любом рефакторинге и не
могут быть эталоном; для просмотра структуры ядра используйте
`--tus full` (диагностический вывод счётчиков без сверок).
"""

import argparse
import json
import os
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)
import scan_symbols as s  # noqa: E402

REPO_ROOT = s.REPO_ROOT

# Инварианты генератора на фиксированных фикстурах. Фикстуры — часть
# тестов сканера: меняются только вместе с самим сканером, поэтому
# дрейф любого из счётчиков относительно этих минимумов означает
# регрессию генератора (потерю функций/типов/макросов/блоков/рёбер),
# а не изменение ядра.
FIXTURE_INVARIANTS = {
    "functions": 4,
    "types": 2,
    "macros": 4,
    "blocks": 10,
    "edges": 3,
}


def scan_fixtures(args) -> dict:
    """Сканирует fixture-файлы, возвращает счётчики собранной карты."""
    col = s.Collector()
    for fx in ("scan_fixture.c", "scan_fixture.cxx"):
        path = os.path.join(TOOLS_DIR, "..", "tests", "fixtures", fx)
        import subprocess
        p = subprocess.run(
            [args_clang(), "-Xclang", "-ast-dump=json", "-fsyntax-only",
             path], capture_output=True, text=True)
        ast = json.loads(p.stdout)
        col.default_file = os.path.abspath(path)
        col.tu_dir = os.path.dirname(os.path.abspath(path))
        col._loc_file = None
        s.walk(ast, col)
        s.scan_macros(args_clang(), fx, {
            "command": "", "file": os.path.abspath(path),
            "directory": col.tu_dir}, col, s.ScanConfig())
    s.finalize_blocks(col)
    got = {
        "functions": len(col.functions),
        "types": len(col.types),
        "macros": len(col.macros),
        "blocks": sum(len(f["blocks"]) for f in col.functions.values()),
        "edges": len(col.edges),
    }
    return got


def report(got: dict, checks: list, checks_ok: list) -> int:
    failed = []
    for name, ok in zip(checks, checks_ok):
        print(("PASS  " if ok else "FAIL  ") + name)
        if not ok:
            failed.append(name)
    if failed:
        print("validator: FAILED (%d check(s))" % len(failed))
        return 1
    print("validator: OK")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=os.path.join(REPO_ROOT, "_build-scan",
                                                 "compile_commands.json"))
    ap.add_argument("--tus", default="fixture", choices=["fixture", "full"],
                    help="fixture: инварианты генератора на фикстурах "
                         "(гейт); full: диагностический скан ядра "
                         "(счётчики без сверок — легитимно меняются при "
                         "любом рефакторинге ядра)")
    ap.add_argument("--map", default=None,
                    help="(только с --tus full) артефакт для полноты "
                         "вывода; сверок по нему не выполняется")
    args = ap.parse_args()

    if args.tus == "full":
        # Диагностический режим: те же счётчики по ядру, что раньше
        # сверялись с golden. Зависят от объёма ядра, поэтому сверок нет.
        cc = json.load(open(args.cc))
        tus = s.select_tus(cc)
        cfg = s.ScanConfig()
        col = s.Collector()
        errors = {}
        for name, entry in tus:
            _, ast, err = s.run_tu(args_clang(), name, entry, cfg)
            if ast is None:
                errors[name] = err or "empty AST"
                continue
            if err:
                errors[name] = err
            col.default_file = entry["file"]
            col.tu_dir = entry["directory"]
            col._loc_file = None
            s.walk(ast, col)
            s.scan_macros(args_clang(), name, entry, col, cfg)
        s.finalize_blocks(col)
        raw = {**col.functions, **col.types, **col.macros}
        symbols, _ = s.canonicalize_symbols(raw)
        got = {
            "functions": sum(1 for v in symbols.values()
                             if v["kind"] == "function"),
            "types": sum(1 for v in symbols.values()
                         if v["kind"] in ("record", "cxxrecord", "enum",
                                          "typedef")),
            "macros": sum(1 for v in symbols.values() if v["kind"] == "macro"),
            "blocks": sum(len(v["blocks"]) for v in symbols.values()
                          if v["kind"] == "function"),
        }
        try:
            from scan_regions import build_regions, collect_source_files
            got["regions"] = len(build_regions(collect_source_files()))
        except ImportError:
            got["regions"] = None
        if args.map:
            try:
                with open(args.map) as f:
                    json.load(f)
                print("map artifact readable: %s" % args.map)
            except OSError as e:
                print("map artifact unreadable: %s" % e)
        for k, v in sorted(got.items()):
            print("INFO  %s: %s (диагностика ядра; сверок нет)" % (k, v))
        print("validator: OK (diagnostic mode, no golden comparisons)")
        return 0

    got = scan_fixtures(args)
    checks = []
    for key, minimum in sorted(FIXTURE_INVARIANTS.items()):
        checks.append("%s: %d >= %d (fixture invariant)"
                      % (key, got.get(key, 0), minimum))
    checks_ok = [got.get(k, 0) >= m for k, m in sorted(FIXTURE_INVARIANTS.items())]
    return report(got, checks, checks_ok)


def args_clang():
    return os.environ.get("CLANG", "clang")


if __name__ == "__main__":
    sys.exit(main())
