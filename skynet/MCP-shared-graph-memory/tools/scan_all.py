#!/usr/bin/env python3
"""Оркестратор многоплатформенного сканирования карты исходников.

Связывает существующие инструменты в детерминированную последовательность:

    configure → scan (linux/win32[/macos]) → merge → uncovered
             → project → probes → validate → load (pin-бэкап → --replace) → report

Локально воспроизводит то, что делает CI (`.github/workflows/ci-mcp-refactor-map.yml`),
включая кросс-скан win32 через clang → mingw-w64. macOS-артефакт локально
невозможен без osxcross/SDK — берётся из CI (`--artifacts-from-ci <run-id>`).

Каждый конфиг обязан внести символы (fail-loudly, см. REFACTORING-MAP.md).
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
REPO_ROOT = os.path.dirname(os.path.dirname(MODULE_DIR))
ARTIFACTS = os.path.join(TOOLS_DIR, "artifacts")
DEFAULT_DB = os.path.expanduser("~/.local/share/shared-graph-memory/db.mdbx")

BUILD_SCAN = os.path.join(REPO_ROOT, "_build-scan")
COMPILE_DB = os.path.join(BUILD_SCAN, "compile_commands.json")

CMAKE_CONFIGURE = [
    "cmake", "-S", REPO_ROOT, "-B", BUILD_SCAN,
    "-DCMAKE_BUILD_TYPE=Debug",
    "-DMDBX_BUILD_CXX=ON",
    "-DMDBX_ENABLE_TESTS=ON",
    "-DMDBX_BUILD_TOOLS=ON",
    "-DINTERPROCEDURAL_OPTIMIZATION=OFF",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    "-DMDBX_ALLOY_BUILD=ON",
    "-DMDBX_USE_MINCORE=OFF",
]


def _sh(cmd, step, dry_run=False):
    if dry_run:
        print("  [dry-run] %s" % " ".join(str(c) for c in cmd))
        return 0
    print("  → %s" % " ".join(str(c) for c in cmd))
    r = subprocess.run(cmd, cwd=REPO_ROOT)
    if r.returncode != 0:
        sys.stderr.write("scan_all: step '%s' failed (exit %d)\n" % (step, r.returncode))
        raise SystemExit(r.returncode)
    return r.returncode


def _tool(name):
    return os.path.join(TOOLS_DIR, name)


def _scan_cmd(config, target=None, extra=None):
    cmd = [sys.executable, _tool("scan_symbols.py"), "--config", config,
           "--cc", COMPILE_DB,
           "--out", os.path.join(ARTIFACTS, "refactoring-map-%s.json" % config)]
    if target:
        cmd += ["--target", target]
    if extra:
        cmd += ["--extra-flags", extra]
    return cmd


def _artifact(name):
    return os.path.join(ARTIFACTS, name)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--configs", default="linux,win32",
                    help="конфигурации сканирования через запятую (linux, win32, macos)")
    ap.add_argument("--db", default=DEFAULT_DB, help="путь к живой БД (для --load)")
    ap.add_argument("--skip-build", action="store_true",
                    help="пропустить cmake-configure _build-scan; подразумевает, что "
                         "compile_commands.json уже существует (или не нужен — dry-run/тесты)")
    ap.add_argument("--skip-merge", action="store_true")
    ap.add_argument("--skip-project", action="store_true")
    ap.add_argument("--skip-probes", action="store_true")
    ap.add_argument("--skip-validate", action="store_true")
    ap.add_argument("--validate-full", action="store_true",
                    help="дополнительно к fixture запустить validate_map.py --tus full")
    ap.add_argument("--skip-load", action="store_true", help="не писать в БД")
    ap.add_argument("--skip-backup", action="store_true",
                    help="не делать pin-бэкап перед load (по умолчанию делается)")
    ap.add_argument("--skip-uncovered", action="store_true")
    ap.add_argument("--dry-run", action="store_true", help="только печатать команды")
    ap.add_argument("--artifacts-from-ci", default=None, metavar="RUN_ID",
                    help="скачать macOS-артефакт из GitHub Actions (gh run download)")
    ap.add_argument("--merge-extra", default=None, metavar="ARTIFACT",
                    help="доп. артефакт для merge (путь к JSON), напр. из CI")
    args = ap.parse_args(argv)

    os.makedirs(ARTIFACTS, exist_ok=True)
    configs = [c.strip() for c in args.configs.split(",") if c.strip()]

    if not os.path.exists(COMPILE_DB):
        if args.skip_build:
            # вызывающий обещал, что _build-scan уже настроен; в dry-run/тестах
            # этого файла может и не быть — оставляем на совести вызывающего
            pass
        else:
            _sh(CMAKE_CONFIGURE, "configure", args.dry_run)

    # scan per config
    for cfg in configs:
        if cfg == "win32":
            cmd = _scan_cmd(cfg, target="x86_64-w64-windows-gnu")
        elif cfg == "macos":
            macos_art = _artifact("refactoring-map-macos.json")
            if not os.path.exists(macos_art):
                if not args.artifacts_from_ci:
                    sys.stderr.write("scan_all: macOS-скан локально невозможен; "
                                     "используйте --artifacts-from-ci <run-id> или не включайте macos\n")
                    raise SystemExit(1)
                # macOS-артефакт скачивается из CI, локальный скан невозможен
                _sh(["gh", "run", "download", args.artifacts_from_ci,
                     "--name", "refactor-map-macos", "--dir", ARTIFACTS],
                    "download-macos", args.dry_run)
                if os.path.exists(os.path.join(ARTIFACTS, "refactoring-map-macos.json")):
                    shutil.move(os.path.join(ARTIFACTS, "refactoring-map-macos.json"),
                                macos_art)
            continue
        else:
            cmd = _scan_cmd(cfg)
        _sh(cmd, "scan-%s" % cfg, args.dry_run)

    # merge
    if not args.skip_merge:
        merge_inputs = [_artifact("refactoring-map-%s.json" % c) for c in configs]
        missing = [c for c, p in zip(configs, merge_inputs) if not os.path.exists(p)]
        if missing and not args.dry_run:
            # fail-loudly: запрошенный конфиг молча не выпадает из merged —
            # ровно тот класс silent-loss, против которого весь конвейер.
            sys.stderr.write("scan_all: merge: отсутствуют артефакты конфигов: %s; "
                             "запустите их скан или передайте --artifacts-from-ci\n" % missing)
            raise SystemExit(1)
        merge_inputs = [p for p in merge_inputs if os.path.exists(p)]
        if args.merge_extra:
            merge_inputs.append(args.merge_extra)
        if len(merge_inputs) < 2 and not args.dry_run:
            sys.stderr.write("scan_all: для merge нужно ≥2 артефакта, есть: %s\n" % merge_inputs)
            raise SystemExit(1)
        cmd = [sys.executable, _tool("scan_symbols.py"), "--merge"] + merge_inputs + \
              ["--out", _artifact("refactoring-map.json")]
        _sh(cmd, "merge", args.dry_run)

        if not args.skip_uncovered:
            _sh([sys.executable, _tool("scan_uncovered.py"),
                 "--map", _artifact("refactoring-map.json")],
                "uncovered", args.dry_run)

    # project-map
    if not args.skip_project:
        _sh([sys.executable, _tool("scan_project.py"), "--repo", REPO_ROOT,
             "--out", _artifact("project-map.json")], "project", args.dry_run)

    # probes
    if not args.skip_probes:
        _sh([sys.executable, _tool("scan_probes.py"),
             "--src", os.path.join(REPO_ROOT, "src"),
             "--probe-bus", os.path.join(REPO_ROOT, "docs", "engineering", "probe-bus.md"),
             "--out", _artifact("probes.json"), "--dry-run"], "probes", args.dry_run)

    # validate
    merged = _artifact("refactoring-map.json")
    if not args.skip_validate and os.path.exists(merged):
        _sh([sys.executable, _tool("validate_map.py"), "--map", merged, "--tus", "fixture"],
            "validate-fixture", args.dry_run)
        if args.validate_full:
            _sh([sys.executable, _tool("validate_map.py"), "--map", merged, "--tus", "full"],
                "validate-full", args.dry_run)

    # load into DB
    if not args.skip_load and os.path.exists(merged):
        if not args.skip_backup and not args.dry_run:
            # pin-точка перед перезаписью структурного слоя: контрактный бэкап
            _sh([sys.executable, "-m", "mcp.cli", "--path", args.db, "backup"],
                "backup-pin", args.dry_run)
        project = _artifact("project-map.json")
        cmd = [sys.executable, _tool("load_map.py"), "--map", merged, "--db", args.db,
               "--replace"]
        if os.path.exists(project):
            cmd += ["--project", project]
        _sh(cmd, "load", args.dry_run)

    # report
    if os.path.exists(merged):
        with open(merged) as f:
            a = json.load(f)
        print("=== report ===")
        print("configs:", a.get("configs"))
        print("counts:", json.dumps(a.get("counts", {})))
        print("uncovered:", len(a.get("uncovered", [])))


if __name__ == "__main__":
    main()
