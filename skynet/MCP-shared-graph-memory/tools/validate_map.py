#!/usr/bin/env python3
"""validate_map.py — валидатор генератора карты исходников.

Повторно сканирует подмножество TU и сверяет счётчики с эталонным
артефактом (golden snapshot). Назначение: поймать регрессии генератора
(пропали символы/рёбра/блоки, разъехались макросы) при изменении кода.

Сверка по принципу «должно совпасть или быть строго больше»: рефакторинг
только добавляет код, поэтому расхождение в меньшую сторону — ошибка.
"""

import argparse
import json
import os
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)
import scan_symbols as s  # noqa: E402

REPO_ROOT = s.REPO_ROOT


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default=os.path.join(TOOLS_DIR, "artifacts",
                                                  "refactoring-map.json"))
    ap.add_argument("--cc", default=os.path.join(REPO_ROOT, "_build-scan",
                                                 "compile_commands.json"))
    ap.add_argument("--tus", default="fixture", choices=["fixture", "full"],
                    help="fixture: только тестовые фикстуры (быстро)")
    args = ap.parse_args()

    with open(args.map) as f:
        golden = json.load(f)

    if args.tus == "fixture":
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
                "directory": col.tu_dir}, col)
        s.finalize_blocks(col)
        got = {
            "functions": len(col.functions),
            "types": len(col.types),
            "macros": len(col.macros),
            "blocks": sum(len(f["blocks"]) for f in col.functions.values()),
        }
        # фикстуры не имеют эталона в артефакте: просто здравые инварианты
        checks = [
            ("functions >= 4", got["functions"] >= 4),
            ("blocks >= 10", got["blocks"] >= 10),
            ("macros >= 2", got["macros"] >= 2),
        ]
    else:
        cc = json.load(open(args.cc))
        tus = s.select_tus(cc)
        col = s.Collector()
        errors = {}
        for name, entry in tus:
            _, ast, err = s.run_tu(args_clang(), name, entry)
            if err or ast is None:
                errors[name] = err
                continue
            col.default_file = entry["file"]
            col.tu_dir = entry["directory"]
            col._loc_file = None
            s.walk(ast, col)
            s.scan_macros(args_clang(), name, entry, col)
        s.finalize_blocks(col)
        got = {
            "functions": len(col.functions),
            "types": len(col.types),
            "macros": len(col.macros),
            "blocks": sum(len(f["blocks"]) for f in col.functions.values()),
        }
        g = golden["counts"]
        # правило «не меньше»: рефакторинг только растёт
        checks = []
        for k in got:
            ok = got[k] >= g[k]
            checks.append(("%s: %d >= %d" % (k, got[k], g[k]), ok))

    failed = [name for name, ok in checks if not ok]
    for name, ok in checks:
        print(("PASS  " if ok else "FAIL  ") + name)
    if failed:
        print("validator: FAILED (%d check(s))" % len(failed))
        return 1
    print("validator: OK")
    return 0


def args_clang():
    return os.environ.get("CLANG", "clang")


if __name__ == "__main__":
    sys.exit(main())