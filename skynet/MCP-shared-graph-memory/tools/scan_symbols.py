#!/usr/bin/env python3
"""scan_symbols.py — карта исходников из clang AST (refactoring-map).

Использует `clang -Xclang -ast-dump=json -fsyntax-only` по записям
compile_commands.json (генерируется configure-only build-диром `_build-scan/`).
Извлекает:
  - symbols: функции / типы / макросы (сигнатура, файл, модуль, диапазон строк);
  - blocks: вложенные блоки кода внутри функций (compound/if/for/while/do)
    со структурным id `symbol#B<N>` и диапазонами строк;
  - call_edges(kind=syntax): вызываемые функции (резолв через DeclRefExpr).

Выход: tools/artifacts/refactoring-map.json (вне git, перегенерируемый).
"""

import argparse
import bisect
import json
import os
import re
import shlex
import subprocess
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
REPO_ROOT = os.path.dirname(os.path.dirname(MODULE_DIR))
SRC_DIR = os.path.join(REPO_ROOT, "src")

_LINE_MARK = re.compile(r'^#\s+(\d+)\s+"([^"]+)"(?:\s+\d)*')
_MACRO_DEF = re.compile(r'^#define\s+([A-Za-z_][A-Za-z0-9_]*)')

BLOCK_KINDS = {"CompoundStmt", "IfStmt", "ForStmt", "WhileStmt", "DoStmt"}
TYPE_KINDS = {"RecordDecl", "EnumDecl", "TypedefDecl", "CXXRecordDecl"}
FUNCTION_KINDS = {"FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl",
                  "CXXDestructorDecl"}

_DROP_ARGS = ("-o", "-c", "-S", "-E", "-MD", "-MMD", "-MP", "-MF", "-MT",
              "-MQ", "-fsyntax-only", "-Werror", "-Wno-error", "-pthread")


def sanitize_args(command: str, source_file: str = None) -> list:
    toks = shlex.split(command)
    out, i, n = [], 1, len(toks)
    while i < n:
        t = toks[i]
        if t in ("-o", "-MF", "-MT", "-MQ"):
            i += 2
            continue
        if t in _DROP_ARGS or any(t.startswith(p) for p in _DROP_ARGS):
            i += 1
            continue
        if source_file and os.path.abspath(t) == os.path.abspath(source_file):
            i += 1  # сам исходник передаётся отдельно (parseTranslationUnit)
            continue
        out.append(t)
        i += 1
    return out


class OffsetIndex:
    """offset -> (line) по файлу (для range.end)."""

    def __init__(self, path: str):
        self.path = path
        self._starts = []
        try:
            with open(path, "rb") as f:
                data = f.read()
            pos = 0
            for chunk in data.split(b"\n"):
                self._starts.append(pos)
                pos += len(chunk) + 1
        except OSError:
            pass

    def line(self, offset):
        if not self._starts:
            return 0
        i = bisect.bisect_right(self._starts, offset)
        return i


_index_cache = {}


def line_index(path):
    if path not in _index_cache:
        _index_cache[path] = OffsetIndex(path)
    return _index_cache[path]


def in_repo(file) -> bool:
    return bool(file) and os.path.commonpath([REPO_ROOT, file]) == REPO_ROOT


def module_of(file) -> str:
    base = os.path.basename(file)
    return base.rsplit(".", 1)[0]


def fn_key(file, name) -> str:
    return "fn:%s:%s" % (module_of(file), name)


_BUILTIN_TYPE_NAMES = frozenset({"max_align_t"})


def is_builtin_type(name: str) -> bool:
    return name.startswith("__") or name in _BUILTIN_TYPE_NAMES


_UNNAMED_SUFFIX = re.compile(r":?~?:?\(unnamed (struct|union) .*\)$")


def clean_cxx_name(name):
    return _UNNAMED_SUFFIX.sub("", name or "").rstrip("::")


class Collector:
    def __init__(self, default_file: str = None, tu_dir: str = None):
        self.default_file = default_file
        self.tu_dir = tu_dir or os.path.dirname(default_file or ".")
        self.functions = {}       # key -> body(dict)
        self.types = {}
        self.macros = {}
        self.blocks = []          # (fn_key, kind, l0, l1, start_off)
        self.edges = []           # (caller_key, callee_name)
        self._cur_fn = None
        self._class_stack = []
        self._method_index = {}   # (short_name, signature) -> qualified name
        self._loc_file = None     # текущий файл по цепочке ast-dump loc.file

    def qualify_method(self, node, name) -> str:
        """Квалифицирует C++ метод через стек классов; для out-of-line
        определений (вне класса) резолвится по (имя, сигнатура)."""
        sig = node.get("type", {}).get("qualType", "")
        name = clean_cxx_name(name)
        if self._class_stack:
            qname = "::".join(self._class_stack + [name])
            self._method_index.setdefault((name, sig), qname)
            return qname
        return self._method_index.get((name, sig), name)

    def add_function(self, file, node, name=None):
        name = clean_cxx_name(name or node.get("name"))
        if not name:
            return
        key = fn_key(file, name)
        loc = node.get("loc", {})
        l0 = loc.get("line", 0)
        end = node.get("range", {}).get("end", {})
        l1 = line_index(file).line(end.get("offset", 0))
        has_body = any(c.get("kind") == "CompoundStmt" for c in node.get("inner", []))
        entry = {
            "kind": "function",
            "name": name,
            "signature": node.get("type", {}).get("qualType", ""),
            "file": os.path.relpath(file, REPO_ROOT),
            "module": module_of(file),
            "l0": l0, "l1": l1,
            "is_definition": has_body,
            "blocks": [],
        }
        if key in self.functions:
            old = self.functions[key]
            if has_body and not old["is_definition"]:
                self.functions[key] = entry
                return key
            return None
        self.functions[key] = entry
        return key

    def add_type(self, file, node):
        name = node.get("name")
        if not name or is_builtin_type(name):
            return
        key = "type:%s:%s" % (module_of(file), name)
        if key in self.types:
            return
        loc = node.get("loc", {})
        self.types[key] = {
            "kind": node["kind"].lower().replace("decl", ""),
            "name": name,
            "file": os.path.relpath(file, REPO_ROOT),
            "module": module_of(file),
            "l0": loc.get("line", 0),
            "l1": loc.get("line", 0),
        }

    def add_macro(self, file, node):
        name = node.get("name")
        if not name:
            return
        key = "macro:%s:%s" % (module_of(file), name)
        if key in self.macros:
            return
        loc = node.get("loc", {})
        self.macros[key] = {
            "kind": "macro", "name": name,
            "file": os.path.relpath(file, REPO_ROOT),
            "module": module_of(file),
            "l0": loc.get("line", 0), "l1": loc.get("line", 0),
        }


def callee_name(ce_node):
    def walk(n):
        if n.get("kind") == "MemberExpr" and n.get("name"):
            return n.get("name")
        if n.get("kind") == "DeclRefExpr":
            rd = n.get("referencedDecl")
            if rd and rd.get("kind") in FUNCTION_KINDS:
                return rd.get("name")
        for c in n.get("inner", []):
            r = walk(c)
            if r:
                return r
        return None
    return walk(ce_node)


def _loc_line(loc_dict, col: Collector) -> int:
    """Строка из loc: прямой line > offset (по текущему файлу) >
    expansionLoc (место вызова макроса) > spellingLoc (тело макроса)."""
    if not loc_dict:
        return 0
    line = loc_dict.get("line")
    if line:
        return int(line)
    off = loc_dict.get("offset")
    if off is not None:
        return line_index(col._loc_file or col.default_file).line(off)
    for sub in ("expansionLoc", "spellingLoc"):
        s = loc_dict.get(sub) or {}
        if s.get("line"):
            return int(s["line"])
        f = s.get("file")
        so = s.get("offset")
        if f and so is not None:
            if not os.path.isabs(f):
                f = os.path.join(col.tu_dir, f)
            return line_index(os.path.abspath(f)).line(so)
    return 0


def walk(node, col: Collector):
    kind = node.get("kind")
    loc = node.get("loc", {})
    # clang эмитит `file` только при смене FileID (дельта-кодирование);
    # отслеживаем текущий файл сквозь весь обход.
    f_explicit = (loc.get("file")
                  or loc.get("expansionLoc", {}).get("file")
                  or loc.get("spellingLoc", {}).get("file")
                  or node.get("range", {}).get("begin", {}).get("file"))
    if f_explicit:
        if not os.path.isabs(f_explicit):
            f_explicit = os.path.join(col.tu_dir, f_explicit)
        col._loc_file = os.path.abspath(f_explicit)
    f = col._loc_file or col.default_file

    if f and in_repo(f):
        if kind in FUNCTION_KINDS:
            qname = node.get("name")
            if kind in ("CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl"):
                qname = col.qualify_method(node, qname or "")
            key = col.add_function(f, node, qname)
            if key:
                prev_fn = col._cur_fn
                col._cur_fn = key
                for child in node.get("inner", []):
                    walk(child, col)
                col._cur_fn = prev_fn
                return
        elif kind in TYPE_KINDS:
            if kind == "CXXRecordDecl":
                name = node.get("name")
                if name:
                    col._class_stack.append(name)
                for child in node.get("inner", []):
                    walk(child, col)
                if name:
                    col._class_stack.pop()
                if name:
                    col.add_type(f, node)
                return
            col.add_type(f, node)
        elif kind == "MacroDefinition":
            col.add_macro(f, node)
        elif kind in BLOCK_KINDS and col._cur_fn:
            begin = node.get("range", {}).get("begin", {})
            end = node.get("range", {}).get("end", {})
            start = begin.get("offset") or 0
            l0 = _loc_line(loc or begin, col) or line_index(f).line(start)
            l1 = _loc_line(end, col)
            col.blocks.append((col._cur_fn, kind, l0, l1, start))
        elif kind in ("CallExpr", "CXXMemberCallExpr") and col._cur_fn:
            name = callee_name(node)
            if name:
                col.edges.append((col._cur_fn, name))

    for child in node.get("inner", []):
        walk(child, col)


def resolve_edges(col: Collector) -> list:
    by_name = {}
    by_short = {}
    for key, fn in col.functions.items():
        by_name.setdefault(fn["name"], []).append(key)
        by_short.setdefault(fn["name"].rsplit("::", 1)[-1], []).append(key)
    out = []
    for caller, name in col.edges:
        cands = by_name.get(name) or by_short.get(name)
        resolved, ambiguity = None, False
        if cands:
            if len(cands) == 1:
                resolved = cands[0]
            else:
                same_mod = [k for k in cands if k.rsplit(":", 1)[0] == caller.rsplit(":", 1)[0]]
                if len(same_mod) == 1:
                    resolved = same_mod[0]
                else:
                    resolved = cands[0]
                    ambiguity = True
        out.append({
            "caller": caller, "callee": resolved or name,
            "resolved": resolved is not None, "ambiguous": ambiguity,
            "kind": "syntax",
        })
    return out


def finalize_blocks(col: Collector):
    per_fn = {}
    for fn_key, kind, l0, l1, start in col.blocks:
        per_fn.setdefault(fn_key, []).append((start, kind, l0, l1))
    for fn_key, lst in per_fn.items():
        fn = col.functions.get(fn_key)
        if not fn:
            continue
        lst.sort()
        fn["blocks"] = [{"id": "B%d" % (i + 1), "kind": k.lower(),
                         "l0": l0, "l1": l1} for i, (_, k, l0, l1) in enumerate(lst)]


def scan_macros(clang, tu, entry, col: Collector):
    """Извлекает #define из репозитория через `clang -E -dD`.
    ast-dump не эмитит макросы вообще; -dD даёт их вместе с
    # <line> "file"-маркерами для привязки к файлу."""
    cmd = [clang, "-E", "-dD"] + sanitize_args(entry["command"],
                                               entry["file"]) + [entry["file"]]
    proc = subprocess.run(cmd, cwd=entry["directory"],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        return
    cur_file = None
    cur_line = 0
    for line in proc.stdout.splitlines():
        m = _LINE_MARK.match(line)
        if m:
            f = os.path.abspath(os.path.join(entry["directory"], m.group(2)))
            if in_repo(f) and os.path.isfile(f):
                cur_file = f
                cur_line = int(m.group(1))
            else:
                cur_file = None
            continue
        if not cur_file:
            continue
        cur_line += 1
        m = _MACRO_DEF.match(line)
        if m:
            key = "macro:%s:%s" % (module_of(cur_file), m.group(1))
            if key not in col.macros:
                col.macros[key] = {
                    "kind": "macro", "name": m.group(1),
                    "file": os.path.relpath(cur_file, REPO_ROOT),
                    "module": module_of(cur_file),
                    "l0": cur_line, "l1": cur_line,
                }


def run_tu(clang, tu, entry) -> tuple:
    args = sanitize_args(entry["command"], entry["file"])
    cmd = [clang, "-Xclang", "-ast-dump=json", "-Xclang",
           "-detailed-preprocessing-record", "-fsyntax-only"] + args + [entry["file"]]
    proc = subprocess.run(cmd, cwd=entry["directory"],
                          capture_output=True, text=True)
    if proc.returncode != 0 or not proc.stdout:
        return tu, None, proc.stderr[:500]
    try:
        ast = json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        return tu, None, "JSON parse error: %s" % e
    return tu, ast, None


def select_tus(cc) -> list:
    out, seen = [], set()
    for e in cc:
        rel = os.path.relpath(e["file"], REPO_ROOT)
        keep = (rel in ("src/alloy.c", "src/mdbx.c++")
                or (rel.startswith("src/tools/")
                    and os.path.basename(rel) in ("copy.c", "drop.c", "dump.c",
                                                  "load.c", "stat.c")))
        if keep and rel not in seen:
            seen.add(rel)
            out.append(("library" if rel in ("src/alloy.c", "src/mdbx.c++")
                        else "tools", e))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=os.path.join(REPO_ROOT, "_build-scan", "compile_commands.json"))
    ap.add_argument("--out", default=os.path.join(TOOLS_DIR, "artifacts", "refactoring-map.json"))
    ap.add_argument("--clang", default="clang")
    args = ap.parse_args()

    cc = json.load(open(args.cc))
    tus = select_tus(cc)
    col = Collector()
    errors = {}
    t0 = time.time()
    for name, entry in tus:
        _, ast, err = run_tu(args.clang, name, entry)
        if err or ast is None:
            errors[name] = err or "empty AST"
            continue
        col.default_file = entry["file"]
        col.tu_dir = entry["directory"]
        col._loc_file = None
        walk(ast, col)
        scan_macros(args.clang, name, entry, col)
        print("  %-9s %s  (%ds)" % (name, os.path.relpath(entry["file"], REPO_ROOT),
                                    int(time.time() - t0)), file=sys.stderr)

    finalize_blocks(col)
    edges = resolve_edges(col)
    artifact = {
        "platform": os.uname().sysname.lower() + "-" + os.uname().machine,
        "build_config": os.path.basename(os.path.dirname(args.cc)),
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "tus": [os.path.relpath(e["file"], REPO_ROOT) for _, e in tus],
        "errors": errors,
        "counts": {
            "functions": len(col.functions),
            "types": len(col.types),
            "macros": len(col.macros),
            "blocks": sum(len(f["blocks"]) for f in col.functions.values()),
            "edges": len(edges),
            "unresolved_edges": sum(1 for e in edges if not e["resolved"]),
        },
        "symbols": {**col.functions, **col.types, **col.macros},
        "edges": edges,
    }
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(artifact, f, ensure_ascii=False, indent=1)
    print(json.dumps(artifact["counts"], indent=1))
    print("written:", args.out)


if __name__ == "__main__":
    main()