#!/usr/bin/env python3
"""scan_project.py — карта всего репозитория (вне C/C++ AST).

Дополняет refactoring-map (src/**) структурой «где-что лежит и взаимосвязи»
по всем остальным категориям репозитория:

  - cmake:    CMakeLists.txt + cmake/*.cmake — цели, функции/макросы, опции,
              add_subdirectory/include-зависимости, связи цель→исходники;
  - scripts:  *.sh / *.py — функции, import/source, вызовы, env-ключи;
  - docs:     *.md — разделы/заголовки, #anchor, перекрёстные ссылки,
              упоминания путей к файлам/символам;
  - ci:       .github/workflows/*.yml — джобы → шаги/скрипты/тесты;
  - config:   *.json, conanfile.py, VERSION.json и т.п.

Выход: tools/artifacts/project-map.json (вне git):
  {
    "files":   {rel_path: {kind, lang, module, size, deps:[rel_path]}},
    "edges":   [{from, to, kind}],            # file_deps: include/import/source/link/workflow/ref
    "symbols": {fq: {kind, file, l0, l1, ...}}, # test:/script:/doc:/cmake: символы
    "groups":  {group_key: [member_key]},
    "counts":  {...}
  }
"""

import argparse
import json
import os
import re
import sys
import time
from itertools import chain

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
REPO_ROOT = os.path.dirname(os.path.dirname(MODULE_DIR))

_EXCLUDED_DIRS = (".git", "_build", "dist", ".skynet", "__pycache__",
                  "tools/artifacts", "build")
_SKIP_FILES = {".gitignore", "DCO", "LICENSE", "NOTICE", "COPYRIGHT", "VERSION.json"}

_HEADER_RE = re.compile(r"^#{1,4}\s+(.*)$")
_ANCHOR_RE = re.compile(r"(?:^|\s)([a-z0-9_./-]+\.md)(?:#([\w-]+))?", re.I)
_INCLUDE_C = re.compile(r'#include\s*[<"]([^>"]+)[>"]')
_CMAKE_ADDSUB = re.compile(r"add_subdirectory\(\s*([\w./-]+)")
_CMAKE_INCLUDE = re.compile(r"include\(\s*([\w./-]+)")
_CMAKE_FUNC = re.compile(r"^\s*(function|macro)\(\s*([\w]+)")
_CMAKE_TARGET = re.compile(r"^\s*(add_library|add_executable|add_ut|add_test)\s*\(\s*([\w-]+)")
_PY_IMPORT = re.compile(r"^\s*(?:from\s+([\w.]+)\s+import|import\s+([\w.]+))")
_PY_DEF = re.compile(r"^def\s+([\w]+)\s*\(")
_SH_SOURCE = re.compile(r"^\s*\.\s+([^\s]+)|^\s*source\s+([^\s]+)")
_SH_DEF = re.compile(r"^([\w]+)\s*\(\)\s*\{")
_CI_JOB = re.compile(r"^  ([\w-]+):\s*$")


def _is_skippable(root: str, rel: str) -> bool:
    if any(part in rel.split("/") for part in _EXCLUDED_DIRS):
        return True
    base = os.path.basename(rel)
    if base in _SKIP_FILES or base.endswith((".pyc", ".o", ".a", ".so")):
        return True
    return False


def _module_of(rel: str) -> str:
    parts = rel.split("/")
    return parts[0] if len(parts) > 1 else "_root"


def classify(rel: str) -> tuple:
    base = os.path.basename(rel)
    if rel.startswith("cmake/") or rel.endswith(".cmake") or base == "CMakeLists.txt":
        return "cmake", "cmake"
    if rel.startswith(".github/"):
        return "ci", "yaml"
    if base.endswith(".sh"):
        return "script", "sh"
    if base.endswith(".py"):
        return "script", "py"
    if base.endswith(".md"):
        return "doc", "md"
    if base.endswith((".yml", ".yaml")):
        return "config", "yaml"
    if base.endswith(".json"):
        return "config", "json"
    if base.endswith((".c", ".c++", ".h", ".h++", ".cc", ".cpp")):
        return "code", "c" if base.endswith(".c") else "c++"
    if base.endswith(".in"):
        return "config", "cmake-in"
    return "misc", "txt"


def collect_files(root: str) -> dict:
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root)
        # prune исключённых каталогов на месте
        dirnames[:] = [d for d in dirnames if d not in
                       (".git", "_build", "_build-scan", "_build-mcp-shared-graph-memory",
                        "dist", ".skynet", "__pycache__", "node_modules", ".venv")]
        for fname in sorted(filenames):
            rel = os.path.join(rel_dir, fname) if rel_dir != "." else fname
            if _is_skippable(root, rel):
                continue
            abspath = os.path.join(dirpath, fname)
            kind, lang = classify(rel)
            try:
                size = os.path.getsize(abspath)
            except OSError:
                size = 0
            out[rel] = {"kind": kind, "lang": lang, "module": _module_of(rel),
                        "size": size}
    return out


def scan_doc_deps(rel: str, root: str, files: dict) -> list:
    """deps для .md: markdown-ссылки `](path)` и упоминания путей файлов."""
    deps = []
    path = os.path.join(root, rel)
    try:
        with open(path, errors="replace") as fh:
            text = fh.read()
    except OSError:
        return deps
    # 1) markdown-ссылки на .md
    for m in _ANCHOR_RE.finditer(text):
        ref = m.group(1)
        norm = ref
        while norm.startswith("../"):
            norm = norm[3:]
        norm = norm.lstrip("./")
        if norm in files:
            deps.append(norm)
        elif "docs/" + norm in files:
            deps.append("docs/" + norm)
    # 2) упоминания любых файлов репозитория (медленный путь: по образцу)
    for cand_rel in files:
        if cand_rel == rel:
            continue
        cand = cand_rel.split("/")[-1]
        # имя файла без расширения и расширение — слишком шумно; ищем полный
        # относительный путь или `<repo>/path`
        if cand_rel in text or ("/" + cand_rel) in text or (
                "`" + cand_rel + "`") in text:
            deps.append(cand_rel)
    return sorted(set(deps))[:60]


def scan_cmake_deps(rel: str, root: str, files: dict) -> list:
    deps = []
    path = os.path.join(root, rel)
    try:
        with open(path, errors="replace") as fh:
            text = fh.read()
    except OSError:
        return deps
    for m in chain(_CMAKE_ADDSUB.finditer(text), _CMAKE_INCLUDE.finditer(text)):
        ref = m.group(1).strip('"')
        cand = os.path.normpath(os.path.join(os.path.dirname(rel), ref))
        if cand in files:
            deps.append(cand)
    for m in _INCLUDE_C.finditer(text):
        ref = m.group(1)
        # include в CMake-контексте — обычно заголовки проекта
        for cand in ("include/" + ref, ref):
            if cand in files:
                deps.append(cand)
    return sorted(set(deps))


def scan_script_deps(rel: str, root: str, files: dict) -> list:
    deps = []
    path = os.path.join(root, rel)
    try:
        with open(path, errors="replace") as fh:
            text = fh.read()
    except OSError:
        return deps
    if rel.endswith(".py"):
        for m in _PY_IMPORT.finditer(text):
            mod = (m.group(1) or m.group(2))
            cand = mod.replace(".", "/")
            if cand + ".py" in files:
                deps.append(cand + ".py")
            elif cand in files:
                deps.append(cand)
    elif rel.endswith(".sh"):
        for m in _SH_SOURCE.finditer(text):
            ref = (m.group(1) or m.group(2))
            if ref in files:
                deps.append(ref)
    return sorted(set(deps))


def scan_symbols(rel: str, root: str, files: dict) -> dict:
    """Символы для док/скриптов/cmake/ci (тестовые/скриптовые/доковые)."""
    syms = {}
    kind, lang = files.get(rel, {}).get("kind"), files.get(rel, {}).get("lang")
    path = os.path.join(root, rel)
    try:
        with open(path, errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return syms
    if kind == "doc":
        for i, ln in enumerate(lines, 1):
            m = _HEADER_RE.match(ln.strip())
            if m:
                title = re.sub(r"[^A-Za-zА-Яа-я0-9 ]", "", m.group(1)).strip().lower()
                title = re.sub(r"\s+", "-", title)[:40]
                syms["doc:%s#%s" % (rel, title)] = {
                    "kind": "section", "file": rel, "l0": i, "l1": i,
                    "name": m.group(1)}
    elif kind == "script" and lang == "py":
        for i, ln in enumerate(lines, 1):
            m = _PY_DEF.match(ln)
            if m:
                syms["script:%s#%s" % (rel, m.group(1))] = {
                    "kind": "function", "file": rel, "l0": i, "l1": i,
                    "name": m.group(1)}
    elif kind == "script" and lang == "sh":
        for i, ln in enumerate(lines, 1):
            m = _SH_DEF.match(ln)
            if m:
                syms["script:%s#%s" % (rel, m.group(1))] = {
                    "kind": "function", "file": rel, "l0": i, "l1": i,
                    "name": m.group(1)}
    elif kind == "cmake":
        for i, ln in enumerate(lines, 1):
            for rx, skind in ((_CMAKE_FUNC, "function"), (_CMAKE_TARGET, "target")):
                m = rx.match(ln)
                if m:
                    name = m.group(2)
                    syms["cmake:%s#%s" % (rel, name)] = {
                        "kind": skind, "file": rel, "l0": i, "l1": i,
                        "name": name}
    return syms


def deps_for(rel: str, root: str, files: dict) -> list:
    kind = files.get(rel, {}).get("kind")
    if kind == "doc":
        return scan_doc_deps(rel, root, files)
    if kind == "cmake":
        return scan_cmake_deps(rel, root, files)
    if kind == "script":
        return scan_script_deps(rel, root, files)
    return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=REPO_ROOT)
    ap.add_argument("--out", default=os.path.join(TOOLS_DIR, "artifacts",
                                                  "project-map.json"))
    args = ap.parse_args()

    root = os.path.realpath(args.repo)
    t0 = time.time()
    files = collect_files(root)
    edges = []
    symbols = {}
    groups = {}
    for rel, meta in sorted(files.items()):
        deps = deps_for(rel, root, files)
        if deps:
            meta["deps"] = deps
            for d in deps:
                edges.append({"from": rel, "to": d, "kind": "file"})
        syms = scan_symbols(rel, root, files)
        if syms:
            symbols.update(syms)
            for key in syms:
                groups.setdefault("group:kind:%s" % meta["kind"], set()).add(key)
    # группы по категориям файлов
    for rel, meta in files.items():
        groups.setdefault("group:file-kind:%s" % meta["kind"], set()).add("file:" + rel)

    by_kind = {}
    for meta in files.values():
        by_kind[meta["kind"]] = by_kind.get(meta["kind"], 0) + 1
    counts = {
        "files": len(files),
        "files_by_kind": by_kind,
        "edges": len(edges),
        "symbols": len(symbols),
    }
    artifact = {
        "platform": os.uname().sysname.lower() + "-" + os.uname().machine,
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "counts": counts,
        "files": files,
        "edges": edges,
        "symbols": symbols,
        "groups": {k: sorted(v) for k, v in groups.items()},
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as fh:
        json.dump(artifact, fh, ensure_ascii=False, indent=1)
    print(json.dumps(counts, indent=1))
    print("written:", args.out, "(%.1fs)" % (time.time() - t0))


if __name__ == "__main__":
    sys.exit(main())