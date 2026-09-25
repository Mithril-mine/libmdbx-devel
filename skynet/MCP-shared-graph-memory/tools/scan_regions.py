#!/usr/bin/env python3
"""scan_regions.py — #if-дерево (cond_regions) карты исходников.

Анализирует условную компиляцию БЕЗ компиляции: по каждому файлу кодовой
базы строит дерево регионов #if/#ifdef/#ifndef/#elif/#else/#endif.

Для каждого региона (ветки) фиксирует:
  - id: `region:{module}:{n}` — стабильный анкор (n — порядковый номер
    директивы-открытия в файле);
  - cond: нормализованное условие (#if A → "A", #ifdef X → "defined(X)",
    #ifndef X → "!defined(X)");
  - kind: if/ifdef/ifndef/elif/else;
  - l0/l1: диапазон строк ветки;
  - depth: глубина вложенности;
  - parent / block_id: родительская ветка и общий id if-блока;
  - location: "inner-function" | "definition-gating" — находится ли ветка
    внутри тела функции или гейтит определения (по стеку скобок);
  - cls: platform | option | technical — класс условия.

Выход: JSON-артефакт (тот же, что у scan_symbols) с разделом "regions".
"""

import argparse
import json
import os
import re
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
REPO_ROOT = os.path.dirname(os.path.dirname(MODULE_DIR))
SRC_DIR = os.path.join(REPO_ROOT, "src")

_DIRECTIVE = re.compile(r'^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b\s*(.*)$')

_PLATFORM_TERMS = (
    "IS_WINDOWS", "_WIN32", "_WIN64", "__WINDOWS__", "__CYGWIN__",
    "__MINGW", "__linux__", "__gnu_linux__", "__ANDROID__", "__APPLE__",
    "__MACH__", "__FreeBSD__", "__NetBSD__", "__OpenBSD__", "__sun",
    "__SVR4", "__svr4__", "__CODEGEARC__", "__TOS_WIN__", "__MSC_VER",
    "__GNUC__", "__clang__", "__has_attribute", "__has_builtin",
    "__has_extension", "__has_feature", "__has_cpp_attribute",
    "__ARM_NEON", "__aarch64__", "__x86_64__", "__i386__", "__powerpc__",
    "__sparc__", "__s390x__", "__riscv", "__wasm", "__EMSCRIPTEN__",
    "__ORBIS__", "__PROSPERO__", "_M_IX86", "_M_X64", "_M_ARM64",
    "_MSC_FULL_VER", "_MSVC_LANG", "__FAST_MATH__", "__SANITIZE_ADDRESS__",
    "__SANITIZE_THREAD__", "__SANITIZE_UNDEFINED__", "__OPTIMIZE__",
    "ENABLE_GPROF", "ENABLE_GCOV", "ENABLE_ASAN", "ENABLE_UBSAN",
    "ENABLE_MEMCHECK", "ENABLE_DTRACE", "ENABLE_SYSTEMTAP", "SYSCTL_LEGACY",
)

_OPTION_TERMS = (
    "MDBX_", "NDEBUG", "ALLOC_EXACTLY", "LIBMDBX_NO_EXPORTS",
    "LTO_ENABLED", "MDBX_TXN_", "TROUBLE_PROVIDE_",
)

_TECHNICAL_TERMS = (
    "MDBX_BUILD_SHARED_LIBRARY", "MDBX_BUILD_TOOLS", "MDBX_BUILD_CXX",
    "MDBX_BUILD_TEST", "MDBX_WITHOUT_MSVC_CRT", "MDBX_MANAGE_BUILD_FLAGS",
)


def classify_condition(cond: str) -> str:
    """Класс условия: platform | option | technical | unknown."""
    if any(t in cond for t in _PLATFORM_TERMS):
        return "platform"
    if any(t in cond for t in _TECHNICAL_TERMS):
        return "technical"
    if any(t in cond for t in _OPTION_TERMS):
        return "option"
    return "unknown"


def normalize_cond(kind: str, cond: str) -> str:
    """Нормализует условие ветки к каноническому виду."""
    cond = cond.strip()
    if kind == "ifdef":
        m = re.match(r'([A-Za-z_]\w*)', cond)
        return "defined(%s)" % m.group(1) if m else "defined(%s)" % cond
    if kind == "ifndef":
        m = re.match(r'([A-Za-z_]\w*)', cond)
        return "!defined(%s)" % m.group(1) if m else "!defined(%s)" % cond
    return cond


class RegionBuilder:
    """Строит дерево регионов одного файла."""

    def __init__(self, filepath: str):
        self.filepath = filepath
        self.module = os.path.splitext(os.path.basename(filepath))[0]
        self.regions = []
        self._brace = 0     # счётчик { } для location
        self._inside_fn = False
        self._last_brace_line = 0

    def _count_braces(self, line: str) -> None:
        """Обновляет состояние внутри-функции по строке кода."""
        stripped = line.strip()
        if not stripped or stripped.startswith("#") or stripped.startswith(
                ("//", "/*", "*")):
            return
        # сигнатура функции: строка заканчивается на '{'
        if not self._inside_fn and self._brace == 0 and re.search(
                r'\)\s*\{$', stripped):
            self._inside_fn = True
        self._brace += stripped.count("{") - stripped.count("}")
        if self._brace <= 0 and self._inside_fn:
            self._inside_fn = False
            self._brace = 0

    def parse(self, lines):
        depth = 0
        seq = 0
        n_lines = len(lines)
        # state стека блоков: каждый элемент = dict с текущей веткой
        stack = []
        for ln, raw in enumerate(lines, 1):
            m = _DIRECTIVE.match(raw)
            if not m:
                self._count_braces(raw)
                continue
            kind, cond = m.group(1), m.group(2)

            if kind == "endif":
                if stack:
                    blk = stack.pop()
                    self._close_branch(blk, ln)
                depth = max(depth - 1, 0)
                continue

            if kind in ("else", "elif"):
                if stack:
                    blk = stack[-1]
                    self._close_branch(blk, ln)
                    prev = blk["branch"]
                    cond_n = ("!%s" % prev["cond"] if kind == "else"
                              else normalize_cond("if", cond))
                    seq += 1
                    rid = "region:%s:%d" % (self.module, seq)
                    reg = {
                        "id": rid,
                        "file": os.path.relpath(self.filepath, REPO_ROOT),
                        "module": self.module,
                        "cond": cond_n,
                        "raw_cond": cond.strip()[:80],
                        "kind": kind,
                        "l0": ln,
                        "l1": None,
                        "depth": depth,
                        "parent": blk["parent"],
                        "block_id": blk["block_id"],
                        "location": ("inner-function" if self._inside_fn
                                     else "definition-gating"),
                        "cls": classify_condition(cond),
                    }
                    self.regions.append(reg)
                    blk["branch"] = reg
                continue

            # #if / #ifdef / #ifndef — открываем блок и первую ветку
            seq += 1
            rid = "region:%s:%d" % (self.module, seq)
            depth += 1
            parent = stack[-1]["block_id"] if stack else None
            reg = {
                "id": rid,
                "file": os.path.relpath(self.filepath, REPO_ROOT),
                "module": self.module,
                "cond": normalize_cond(kind, cond),
                "raw_cond": cond.strip()[:80],
                "kind": kind,
                "l0": ln,
                "l1": None,
                "depth": depth,
                "parent": parent,
                "block_id": rid,
                "location": ("inner-function" if self._inside_fn
                             else "definition-gating"),
                "cls": classify_condition(cond),
            }
            self.regions.append(reg)
            stack.append({"block_id": rid, "parent": parent, "branch": reg})
        # незакрытые блоки (некорректный файл) — пометить
        for blk in stack:
            self._close_branch(blk, n_lines + 1)
        return [r for r in self.regions if r.get("l1") is not None or True]

    @staticmethod
    def _close_branch(blk, end_line):
        """Закрывает текущую ветку блока на строке end_line (директива)."""
        branch = blk.get("branch")
        if branch and branch.get("l1") is None:
            branch["l1"] = end_line


def build_regions(files) -> list:
    """Строит дерево регионов для списка файлов."""
    out = []
    for path in files:
        with open(path, errors="replace") as f:
            lines = f.read().splitlines()
        rb = RegionBuilder(path)
        out.extend(rb.parse(lines))
    return out


def collect_source_files():
    """Список .c/.c++ файлов кодовой базы (по включаетам alloy.c)."""
    files = []
    alloy = os.path.join(SRC_DIR, "alloy.c")
    if os.path.exists(alloy):
        for m in re.finditer(r'#include "([^"]+)"', open(alloy).read()):
            files.append(os.path.join(SRC_DIR, m.group(1)))
    else:
        for root, _, fs in os.walk(SRC_DIR):
            for f in fs:
                if f.endswith((".c", ".c++")):
                    files.append(os.path.join(root, f))
    files.append(os.path.join(SRC_DIR, "mdbx.c++"))
    seen, out = set(), []
    for f in files:
        if os.path.exists(f) and f not in seen:
            seen.add(f)
            out.append(f)
    return sorted(out)


def render_tree(regions, module=None, max_depth=None):
    """Текстовое дерево регионов для визуальной проверки вложенности."""
    by_id = {r["id"]: r for r in regions}
    roots = sorted([r for r in regions if r["parent"] is None
                    and r["kind"] not in ("elif", "else")],
                   key=lambda r: (r["file"], r["l0"]))
    lines = []
    for root in roots:
        if module and root["module"] != module:
            continue
        _render_branch(root, regions, by_id, lines, 0, max_depth)
    return "\n".join(lines)


def _render_branch(reg, regions, by_id, out, depth, max_depth):
    if max_depth is not None and depth > max_depth:
        return
    pad = "  " * depth
    out.append("%s%s [%d-%d] %s %s loc=%s" % (
        pad, reg["id"].rsplit(":", 1)[-1], reg["l0"], reg.get("l1"),
        reg["kind"].ljust(5), reg["cond"][:50], reg["location"]))
    # ветки того же блока (elif/else) и дочерние блоки
    siblings = [r for r in regions
                if r["block_id"] == reg["block_id"] and r["id"] != reg["id"]]
    for s in sorted(siblings, key=lambda r: r["l0"]):
        out.append("%s  %s-> [%d-%d] %s %s loc=%s" % (
            pad, "|", s["l0"], s.get("l1"), s["kind"].ljust(5),
            s["cond"][:50], s["location"]))
    children = sorted([r for r in regions
                       if r["parent"] == reg["block_id"] and r["kind"] in
                       ("if", "ifdef", "ifndef")], key=lambda r: r["l0"])
    for c in children:
        _render_branch(c, regions, by_id, out, depth + 1, max_depth)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(TOOLS_DIR, "artifacts",
                                                  "refactoring-map.json"))
    ap.add_argument("--stats", action="store_true", help="только сводка")
    ap.add_argument("--tree", metavar="MODULE", nargs="?",
                    const="", help="дерево регионов модуля (или всех)")
    ap.add_argument("--max-depth", type=int, default=None)
    args = ap.parse_args()

    regions = build_regions(collect_source_files())
    if args.stats:
        from collections import Counter
        print("регионов всего:", len(regions))
        print("по kind:", dict(Counter(r["kind"] for r in regions)))
        print("по cls:", dict(Counter(r["cls"] for r in regions)))
        print("по location:", dict(Counter(r["location"] for r in regions)))
        print("max depth:", max((r["depth"] for r in regions), default=0))
        return

    if args.tree is not None:
        mod = args.tree or None
        print(render_tree(regions, module=mod, max_depth=args.max_depth))
        return

    artifact = {}
    if os.path.exists(args.out):
        with open(args.out) as f:
            artifact = json.load(f)
    artifact["regions"] = regions
    with open(args.out, "w") as f:
        json.dump(artifact, f, ensure_ascii=False, indent=1)
    print("regions: %d -> %s" % (len(regions), args.out))


if __name__ == "__main__":
    main()