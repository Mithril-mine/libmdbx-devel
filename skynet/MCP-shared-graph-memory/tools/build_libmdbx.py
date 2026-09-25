#!/usr/bin/env python3
"""Кроссплатформенная сборка libmdbx (из master-дерева рядом) для Python-модуля.

Собирает shared-библиотеку libmdbx независимо от основного CMake-проекта
(отдельный build-каталог, без тестов/CXX/LTO) и кладёт артефакт в
``mcp_memory/_lib/``. Рантайм находит её автоматически (см. libmdbx.load_library).

Использование:
    python3 tools/build_libmdbx.py [--src <корень репо libmdbx>] [--build-dir B]
                                   [--output DIR] [--skip]

Окружение:
    LIBMDBX_SRC_DIR        корень репо (альтернатива --src)
    MDBX_MODULE_CMAKE_ARGS доп. аргументы cmake (например "-A x64 -T v143")
    CMAKE_GENERATOR        принудительный генератор
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

MODULE_ROOT = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
REPO_ROOT = os.path.realpath(os.path.join(MODULE_ROOT, "..", ".."))
DEFAULT_BUILD_DIR = os.path.join(REPO_ROOT, "_build-mcp-memory")
OUTPUT_DIR = os.path.join(MODULE_ROOT, "mcp_memory", "_lib")

ARTIFACT_PATTERNS = (
    "libmdbx.so",
    "libmdbx.dylib",
    "libmdbx.dll",
    "mdbx.dll",
)

CMAKE_BASE = [
    "-DMDBX_BUILD_CXX:BOOL=OFF",
    "-DMDBX_ENABLE_TESTS:BOOL=OFF",
    "-DMDBX_BUILD_SHARED_LIBRARY:BOOL=ON",
    "-DINTERPROCEDURAL_OPTIMIZATION:BOOL=OFF",
]


def _run(cmd, cwd=None):
    print("+ %s" % " ".join(cmd), file=sys.stderr)
    subprocess.check_call(cmd, cwd=cwd)


def _have(cmd):
    return shutil.which(cmd) is not None


def _find_artifact(build_dir):
    for root, _dirs, files in os.walk(build_dir):
        for name in files:
            if name in ARTIFACT_PATTERNS:
                return os.path.join(root, name)
    return None


def _git_describe(src):
    try:
        out = subprocess.run(
            ["git", "-C", src, "describe", "--tags", "--always"],
            capture_output=True, text=True, check=True,
        )
        return out.stdout.strip()
    except Exception:
        return "unknown"


def build(src: str, build_dir: str, output: str, extra_args: list = None) -> str:
    src = os.path.realpath(src)
    if not os.path.isfile(os.path.join(src, "CMakeLists.txt")):
        raise SystemExit("FATAL: %r не похож на корень репо libmdbx (нет CMakeLists.txt)" % src)

    os.makedirs(build_dir, exist_ok=True)

    system = sys.platform
    generator_args = []
    if os.environ.get("CMAKE_GENERATOR"):
        generator_args = ["-G", os.environ["CMAKE_GENERATOR"]]
    elif system == "win32":
        # Visual Studio multi-config; MinGW fallback.
        if _have("ninja") and _have("cl"):
            generator_args = ["-G", "Ninja"]
        else:
            generator_args = ["-G", "Visual Studio 17 2022"]
    else:
        if _have("ninja"):
            generator_args = ["-G", "Ninja"]

    toolchain = []
    if _have("ccache"):
        toolchain = [
            "-DCMAKE_C_COMPILER_LAUNCHER=ccache",
        ]
    extra = list(extra_args or [])
    env_extra = os.environ.get("MDBX_MODULE_CMAKE_ARGS", "")
    if env_extra:
        extra += env_extra.split()

    configure = ["cmake", "-S", src, "-B", build_dir, "-DCMAKE_BUILD_TYPE=Release"]
    configure += generator_args + CMAKE_BASE + toolchain + extra
    _run(configure)

    build_cmd = ["cmake", "--build", build_dir, "--target", "mdbx"]
    if system == "win32" and "Ninja" not in " ".join(generator_args):
        build_cmd += ["--config", "Release"]
    _run(build_cmd)

    artifact = _find_artifact(build_dir)
    if not artifact:
        raise SystemExit("FATAL: артефакт libmdbx не найден в %r" % build_dir)

    os.makedirs(output, exist_ok=True)
    dest = os.path.join(output, os.path.basename(artifact))
    shutil.copyfile(artifact, dest)
    with open(os.path.join(output, "VERSION.txt"), "w") as f:
        f.write("%s\n" % _git_describe(src))
    print("shared-graph-memory: libmdbx => %s (%s)" % (dest, _git_describe(src)))
    return dest


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(prog="build_libmdbx")
    ap.add_argument("--src", default=os.environ.get("LIBMDBX_SRC_DIR") or REPO_ROOT)
    ap.add_argument("--build-dir", default=DEFAULT_BUILD_DIR)
    ap.add_argument("--output", default=OUTPUT_DIR)
    ap.add_argument("--skip", action="store_true",
                    help="не собирать (полезно при pip install с уже собранным _lib)")
    args, unknown = ap.parse_known_args(argv)
    if args.skip:
        print("shared-graph-memory: сборка libmdbx пропущена (--skip)")
        return 0
    build(args.src, args.build_dir, args.output, extra_args=unknown)
    return 0


if __name__ == "__main__":
    sys.exit(main())