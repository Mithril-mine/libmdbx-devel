#!/usr/bin/env python3
"""validate_map.py — валидатор генератора карты исходников.

Повторно сканирует подмножество TU и сверяет счётчики со статичным
эталоном tests/golden/map-counts.json (не с самим артефактом — иначе
тавтология). Назначение: поймать регрессии генератора (пропали
символы/рёбра/блоки, разъехались макросы) при изменении кода.

Сверка по принципу «должно совпасть или быть строго больше»: рефакторинг
только добавляет код, поэтому расхождение в меньшую сторону — ошибка.

`--refresh-golden` обновляет эталон после осознанных изменений.
"""

import argparse
import json
import os
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)
import scan_symbols as s  # noqa: E402

REPO_ROOT = s.REPO_ROOT
GOLDEN_PATH = os.path.join(os.path.dirname(TOOLS_DIR), "tests", "golden",
                           "map-counts.json")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default=os.path.join(TOOLS_DIR, "artifacts",
                                                  "refactoring-map.json"))
    ap.add_argument("--cc", default=os.path.join(REPO_ROOT, "_build-scan",
                                                 "compile_commands.json"))
    ap.add_argument("--tus", default="fixture", choices=["fixture", "full"],
                    help="fixture: только тестовые фикстуры (быстро)")
    ap.add_argument("--refresh-golden", action="store_true",
                    help="обновить tests/golden/map-counts.json по артефакту")
    args = ap.parse_args()

    with open(args.map) as f:
        artifact = json.load(f)
    if args.refresh_golden:
        if "configs" in artifact:
            print("golden: артефакт многоконфигурационный (configs=%s), "
                  "эталон привязан к базовому linux-скану. "
                  "Обновите golden от одно-конфиг артефакта (--config linux)."
                  % artifact["configs"], file=sys.stderr)
            return 2
        golden = {"platform": artifact.get("platform"),
                  "build_config": artifact.get("build_config"),
                  "counts": {k: v for k, v in artifact["counts"].items()
                             if k not in ("coverage_pct", "uncovered_lines")},
                  "generated_by": "scan_symbols.py + scan_regions.py"}
        with open(GOLDEN_PATH, "w") as f:
            json.dump(golden, f, ensure_ascii=False, indent=2)
            f.write("\n")
        print("golden обновлён:", GOLDEN_PATH)
        return 0
    with open(GOLDEN_PATH) as f:
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
                "directory": col.tu_dir}, col, s.ScanConfig())
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
        cfg = s.ScanConfig()
        col = s.Collector()
        errors = {}
        for name, entry in tus:
            _, ast, err = s.run_tu(args_clang(), name, entry, cfg)
            if err or ast is None:
                errors[name] = err
                continue
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
                         if v["kind"] in ("record", "cxxrecord", "enum", "typedef")),
            "macros": sum(1 for v in symbols.values() if v["kind"] == "macro"),
            "blocks": sum(len(v["blocks"]) for v in symbols.values()
                         if v["kind"] == "function"),
        }
        # регионы считаются из исходников (без clang) — точечная проверка
        try:
            from scan_regions import build_regions, collect_source_files
            got["regions"] = len(build_regions(collect_source_files()))
        except ImportError:
            got["regions"] = None
        g = golden["counts"]
        # правило «не меньше»: рефакторинг только растёт
        checks = []
        for k in got:
            if got[k] is None:
                continue
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