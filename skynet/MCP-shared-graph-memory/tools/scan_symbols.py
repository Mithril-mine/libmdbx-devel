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

try:
    from scan_regions import build_regions, collect_source_files
except ImportError:  # при запуске как модуля из tests/
    from tools.scan_regions import build_regions, collect_source_files

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


_REPO_EXCLUDED_DIRS = ("_build-", "dist", ".git", ".skynet")


def in_repo(file) -> bool:
    """Файл внутри репозитория и не в build/служебных директориях."""
    if not file:
        return False
    try:
        if os.path.commonpath([REPO_ROOT, file]) != REPO_ROOT:
            return False
    except ValueError:
        return False
    rel = os.path.relpath(file, REPO_ROOT)
    parts = rel.split(os.sep)
    return not any(p == d or p.startswith(d)
                   for p in parts for d in _REPO_EXCLUDED_DIRS)


def module_of(file) -> str:
    base = os.path.basename(file)
    return base.rsplit(".", 1)[0]


def fn_key(file, name) -> str:
    return "fn:%s:%s" % (module_of(file), name)


_demangle_cache = {}
_demangle_bin = None


def demangle_cpp(mangled):
    """Деманглит C++ имя через c++filt (если доступен).

    Возвращает `mdbx::env::get_context` (без сигнатуры) или None.
    """
    if not mangled or not mangled.startswith("_Z"):
        # C-функции: mangled == обычное имя, деманглинг не нужен
        return None
    global _demangle_bin
    if _demangle_bin is None:
        import shutil
        _demangle_bin = shutil.which("c++filt") or ""
    if not _demangle_bin:
        return None
    if mangled not in _demangle_cache:
        try:
            r = subprocess.run([_demangle_bin, "-n", mangled],
                               capture_output=True, text=True, timeout=5)
            out = r.stdout.strip()
            if out and out != mangled:
                name = out.split("(", 1)[0]
                _demangle_cache[mangled] = name
            else:
                _demangle_cache[mangled] = None
        except Exception:
            _demangle_cache[mangled] = None
    return _demangle_cache[mangled]


_BUILTIN_TYPE_NAMES = frozenset({"max_align_t"})


def is_builtin_type(name: str) -> bool:
    return name.startswith("__") or name in _BUILTIN_TYPE_NAMES


_UNNAMED_SUFFIX = re.compile(r":?~?:?\(unnamed (struct|union) .*\)$")


def clean_cxx_name(name):
    return _UNNAMED_SUFFIX.sub("", name or "").rstrip("::")


class ScanConfig:
    """Конфигурация сканирования (платформа/опции компиляции).

    linux (по умолчанию): флаги берутся из compile_commands как есть.
    Кросс-конфигурации (win32 и т.п.): из compile_commands сохраняются только
    -D/-I (пути к src/_build-scan, версия/конфиг), остальное собирается из
    --target/--defines/--includes/--extra-flags.
    """

    def __init__(self, name="linux", target=None, defines=None,
                 includes=None, extra_flags=None, no_detailed_preprocessing=False):
        self.name = name
        self.target = target
        self.defines = shlex.split(defines or "")
        self.includes = shlex.split(includes or "")
        self.extra_flags = shlex.split(extra_flags or "")
        self.no_detailed_preprocessing = no_detailed_preprocessing
        self._cross = bool(target or defines or includes or extra_flags)

    def args_for(self, entry: dict, source_file: str = None) -> list:
        base = sanitize_args(entry["command"], source_file)
        if not self._cross:
            return base
        kept = [t for t in base
                if t.startswith("-D") or t.startswith("-I")
                or t.startswith("-isystem")
                or t.startswith("-Wno-") or t == "-pthread"]
        out = []
        if self.target:
            out.append("--target=%s" % self.target)
        out += kept
        for d in self.defines:
            out.append("-D%s" % d if not d.startswith("-D") else d)
        for inc in self.includes:
            out.append("-I%s" % inc)
        out += self.extra_flags
        return out


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
        self._scope_stack = []   # имена namespace'ов и классов (для fallback-квалификации)
        self._method_index = {}  # (short_name, signature) | short_name -> qname
        self._loc_file = None     # текущий файл по цепочке ast-dump loc.file
        self._block_ctx = []      # счётчики уровней вложенности блоков
        self._block_paths = {}    # временный ключ -> path-based id

    def qualify_method(self, node, name) -> str:
        """Квалифицирует C++ метод через стек namespace'ов/классов; для
        out-of-line определений (вне класса) резолвится по mangledName
        (демангл), иначе по индексу (имя, сигнатура).

        Демангл mangledName — точный источник (полная квалификация, включая
        namespace; решает коллизии impl_env/impl_txn/impl_cursor), но требует
        внешний c++filt (есть не везде, например на macOS его нет) — тогда
        используем структурный fallback по стеку областей видимости.
        """
        sig = node.get("type", {}).get("qualType", "")
        name = clean_cxx_name(name)
        # точный источник: mangledName (содержит полную квалификацию,
        # включая namespace; решает коллизии impl_env/impl_txn/impl_cursor)
        mangled = node.get("mangledName")
        demangled = demangle_cpp(mangled) if mangled else None
        if demangled:
            # убираем ведущий глобальный scope ("::mdbx::env::get_context")
            return demangled.lstrip(":")
        # структурный fallback (нет demangler'а): сначала точный индекс
        # inline-объявлений по (имя, сигнатура), затем последний известный
        # qname для короткого имени (покрывает out-of-line определения,
        # где у узла только короткое имя без класса в scope_stack).
        qname = (self._method_index.get((name, sig))
                 or self._method_index.get(name))
        if qname:
            return qname
        if not self._scope_stack:
            return name
        qname = "::".join(self._scope_stack + [name])
        self._method_index[(name, sig)] = qname
        self._method_index[name] = qname
        return qname

    def add_function(self, file, node, name=None):
        name = clean_cxx_name(name or node.get("name"))
        if not name:
            return
        sig = node.get("type", {}).get("qualType", "")
        has_body = any(c.get("kind") == "CompoundStmt" for c in node.get("inner", []))
        key = fn_key(file, name)
        # перегрузки C++: одинаковый qname в одном файле → различить
        # сигнатурой на уровне сырого ключа, чтобы canonicalize_symbols
        # получил оба определения и построил корректные fn:...#sig-hash
        if key in self.functions:
            existing = self.functions[key]
            if existing["signature"] != sig:
                sig_hash = abs(hash(sig)) % 0xFFFF
                key = "%s#%x" % (key, sig_hash)
        loc = node.get("loc", {})
        l0 = loc.get("line", 0)
        end = node.get("range", {}).get("end", {})
        # предпочитаем явную строку конца (может относиться к другому
        # файлу при макро-расширениях — offset тогда ненадёжен)
        l1 = end.get("line") or 0
        if not l1:
            end_off = end.get("offset", 0)
            l1 = line_index(file).line(end_off) if end_off else l0
        if l1 < l0:
            l1 = l0
        is_static = node.get("storageClass") == "static"
        entry = {
            "kind": "function",
            "name": name,
            "signature": sig,
            "file": os.path.relpath(file, REPO_ROOT),
            "module": module_of(file),
            "l0": l0, "l1": l1,
            "is_definition": has_body,
            "static": is_static,
            "mangled": node.get("mangledName"),
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
                prev_ctx = col._block_ctx
                col._cur_fn = key
                col._block_ctx = []
                for child in node.get("inner", []):
                    walk(child, col)
                col._cur_fn = prev_fn
                col._block_ctx = prev_ctx
                return
        elif kind in TYPE_KINDS:
            if kind == "CXXRecordDecl":
                name = node.get("name")
                if name:
                    col._scope_stack.append(name)
                for child in node.get("inner", []):
                    walk(child, col)
                if name:
                    col._scope_stack.pop()
                if name:
                    col.add_type(f, node)
                return
            col.add_type(f, node)
        elif kind == "NamespaceDecl":
            # анонимные и инлайн-namespace в квалификацию не попадают
            name = node.get("name")
            if name and not name.startswith("(") and not name.endswith(")"):
                col._scope_stack.append(name)
            for child in node.get("inner", []):
                walk(child, col)
            if name and not name.startswith("(") and not name.endswith(")"):
                col._scope_stack.pop()
            return
        elif kind == "MacroDefinition":
            col.add_macro(f, node)
        elif kind in BLOCK_KINDS and col._cur_fn:
            begin = node.get("range", {}).get("begin", {})
            end = node.get("range", {}).get("end", {})
            start = begin.get("offset") or 0
            l0 = _loc_line(loc or begin, col) or line_index(f).line(start)
            l1 = _loc_line(end, col)
            if l1 < l0:
                l1 = l0
            # path-based id: путь от корня функции через уровни вложенности.
            # Каждый уровень = число завершённых сиблингов + 1; вырожденные
            # макро-блоки не ломают нумерацию (контекст независим от строк).
            depth = len(col._block_ctx)
            if not col._block_ctx:
                bid = "B%d" % 1
                col._block_ctx.append(1)
            else:
                col._block_ctx[-1] += 1
                bid = "B%s.%d" % (".".join(str(c) for c in col._block_ctx[:-1]),
                                  col._block_ctx[-1])
            col.blocks.append((col._cur_fn, kind, l0, l1, start, bid))
            # дети блока — внутри него: продолжаем с тем же контекстом,
            # увеличивая счётчик только для сиблингов на этом уровне
            prev_ctx = col._block_ctx
            col._block_ctx = list(prev_ctx) + [0]
            for child in node.get("inner", []):
                walk(child, col)
            col._block_ctx = prev_ctx
            return
        elif kind in ("CallExpr", "CXXMemberCallExpr") and col._cur_fn:
            name = callee_name(node)
            if name:
                col.edges.append((col._cur_fn, name))

    for child in node.get("inner", []):
        walk(child, col)


def resolve_edges(col: Collector, symbols=None) -> list:
    """Резолвит call-рёбра по имени callee.

    Если symbols задан (канонизированный словарь) — строит индекс по нему,
    иначе по col.functions.
    """
    syms = symbols or col.functions
    by_name = {}
    by_short = {}
    for key, fn in syms.items():
        if fn.get("kind") != "function":
            continue
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


def canonicalize_symbols(symbols: dict) -> tuple:
    """Канонизация ключей функций: fn:{module}:{name} → fn:{qname}[#sig][@mod].

    Модель идентичности (см. docs/REFACTORING-MAP.md):
      - база: qname (квалифицированное имя; для C++ — из демангла mangledName);
      - перегрузки C++ (одинаковый qname, разные сигнатуры): суффикс #<sig-hash>;
      - TU-static (одинаковый qname+сигнатура в разных файлах): суффикс @<module>;
      - inline-функции хидеров, видимые в нескольких TU: дедуп (один символ),
        extra-определения помечаются `extra_def: true`.

    Возвращает (новые_symbols, mapping_old→new).
    """
    funcs = {k: v for k, v in symbols.items() if v["kind"] == "function"}
    others = {k: v for k, v in symbols.items() if v["kind"] != "function"}

    # группируем определения по qname (сигнатура решается внутри группы)
    groups = {}   # qname -> list[(old_key, entry)]
    for old_key, fn in funcs.items():
        qname = fn["name"]
        groups.setdefault(qname, []).append((old_key, fn))

    new_funcs = {}
    mapping = {}
    for qname, entries in groups.items():
        # кандидат с телом — главный; без тела — декларация (исключаем)
        defs = [(k, e) for k, e in entries if e["is_definition"]]
        decls = [(k, e) for k, e in entries if not e["is_definition"]]
        if not defs:
            # только декларации: сохраняем одну (без тела) — для покрытия API
            for old_key, e in decls:
                base = "fn:%s" % qname
                new_key = base
                # декларации по одному qname могут быть в нескольких файлах
                if len(decls) > 1:
                    new_key = "%s@%s" % (base, e["module"])
                if new_key not in new_funcs:
                    e2 = dict(e); e2["is_definition"] = False
                    new_funcs[new_key] = e2
                mapping[old_key] = new_key
            continue
        # несколько определений?
        modules = {e["module"] for _, e in defs}
        files = {e["file"] for _, e in defs}
        sigs = {e["signature"] for _, e in defs}
        same_sig_all = len(sigs) == 1
        if len(defs) == 1:
            base = "fn:%s" % qname
            new_key = base
            e2 = dict(defs[0][1])
            new_funcs[new_key] = e2
            for old_key, _ in entries:
                mapping[old_key] = new_key
            continue
        if same_sig_all and len(files) == 1:
            # одна функция, видимая в нескольких TU (inline-хидера): дедуп
            best = max(defs, key=lambda ke: (ke[1]["l1"] - ke[1]["l0"]))
            new_key = "fn:%s" % qname
            e2 = dict(best[1])
            extras = [e for k, e in defs if e is not best[1]]
            if extras:
                e2["extra_defs"] = [{"module": e["module"], "file": e["file"],
                                     "l0": e["l0"], "l1": e["l1"]} for e in extras]
            new_funcs[new_key] = e2
            for old_key, _ in entries:
                mapping[old_key] = new_key
            continue
        # несколько определений — разные функции
        if same_sig_all:
            # одинаковая сигнатура в разных файлах: TU-static → @module
            for old_key, e in defs:
                new_key = "fn:%s@%s" % (qname, e["module"])
                new_funcs[new_key] = dict(e)
                mapping[old_key] = new_key
        else:
            # разные сигнатуры: перегрузки (C++) либо случайные совпадения
            # имён — различаем хэшем сигнатуры
            for old_key, e in defs:
                sig_hash = abs(hash(e["signature"])) % 0xFFFF
                new_key = "fn:%s#%x" % (qname, sig_hash)
                new_funcs[new_key] = dict(e)
                mapping[old_key] = new_key
        for old_key, e in decls:
            mapping[old_key] = "fn:%s@%s" % (qname, e["module"])

    out_symbols = {**new_funcs, **others}
    return out_symbols, mapping


def finalize_blocks(col: Collector):
    per_fn = {}
    for fn_key, kind, l0, l1, start, bid in col.blocks:
        per_fn.setdefault(fn_key, []).append((start, kind, l0, l1, bid))
    for fn_key, lst in per_fn.items():
        fn = col.functions.get(fn_key)
        if not fn:
            continue
        # сохраняем порядок обхода (walk уже дал структурные id)
        lst.sort(key=lambda b: b[0])
        fn["blocks"] = [{"id": bid, "kind": k.lower(), "l0": l0, "l1": l1}
                        for _, k, l0, l1, bid in lst]


def scan_macros(clang, tu, entry, col: Collector, cfg: ScanConfig = None):
    """Извлекает #define из репозитория через `clang -E -dD`.
    ast-dump не эмитит макросы вообще; -dD даёт их вместе с
    # <line> "file"-маркерами для привязки к файлу."""
    cfg = cfg or ScanConfig()
    cmd = ([clang, "-E", "-dD"]
           + cfg.args_for(entry, entry["file"]) + [entry["file"]])
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


def run_tu(clang, tu, entry, cfg: ScanConfig = None) -> tuple:
    cfg = cfg or ScanConfig()
    args = cfg.args_for(entry, entry["file"])
    cmd = [clang, "-Xclang", "-ast-dump=json"]
    if not cfg.no_detailed_preprocessing:
        cmd += ["-Xclang", "-detailed-preprocessing-record"]
    cmd += ["-fsyntax-only"] + args + [entry["file"]]
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


# тестовые домены: имя → (префикс, подкаталог)
_TEST_DOMAINS = {"ut": ("ut/", "tests/ut"),
                 "issues": ("issues/", "tests/ut/issues"),
                 "framework": ("framework/", "tests/framework")}
TEST_DOMAIN_NAMES = tuple(_TEST_DOMAINS)


def select_test_tus(domain="ut") -> list:
    """Синтетические записи TU для тестового домена.

    Команда строится по образцу библиотечного TU из compile_commands
    (определения конфига/инклуды) + тестовые дефайны; clang запускается
    как C++. Список файлов читается лениво (при вызове, не при импорте).
    """
    prefix, subdir = _TEST_DOMAINS[domain]
    files = [os.path.relpath(os.path.join(r, f), REPO_ROOT)
             for r, _, ns in os.walk(os.path.join(REPO_ROOT, subdir))
             for f in sorted(ns)
             if f.endswith((".c", ".c++"))]
    if domain == "framework":
        files = [f for f in files
                 if os.path.basename(f) not in ("main.c++",)]
    # образец команды: первый либ-TU из compile_commands
    cc_path = os.path.join(REPO_ROOT, "_build-scan", "compile_commands.json")
    base_defs, base_includes = [], []
    if os.path.exists(cc_path):
        with open(cc_path) as f:
            cc = json.load(f)
        model = next((e for e in cc if e["file"].endswith("mdbx.c++")), None)
        if model:
            toks = shlex.split(model["command"])
            base_defs = [t for t in toks if t.startswith("-D")]
            base_includes = [t for t in toks
                             if t.startswith("-I")
                             and any(d in t for d in ("_build-scan", "/src"))]
        # googletest include: реальные тестовые TU несут его как -isystem;
        # в синтетическую команду тестового домена он нужен явно.
        test_model = next((e for e in cc
                           if "tests/" in e["file"].replace("\\", "/")), None)
        if test_model:
            ttoks = shlex.split(test_model["command"])
            for i, t in enumerate(ttoks):
                if t.startswith("-isystem"):
                    val = t[len("-isystem"):] or (ttoks[i + 1]
                                                  if i + 1 < len(ttoks) else "")
                    if "googletest" in val and not any(
                            "-isystem%s" % val == x for x in base_includes):
                        base_includes.append("-isystem%s" % val)
    else:
        # CI / отсутствие configure-only build-дира: синтетический конфиг
        # (тот же путь, что дал бы CMake; файл может не существовать —
        # тесты смотрят только на форму аргумента, clang не запускается).
        cfg_h = os.path.join(REPO_ROOT, "_build-scan", "config-cmake.h")
        base_defs = ['-DMDBX_CONFIG_H=\\"%s\\"' % cfg_h]
    out = []
    for rel in files:
        abspath = os.path.join(REPO_ROOT, rel)
        defs = []
        for t in base_defs:
            # кавычки теряются при двойном shlex-разборе; для -D с путём
            # (MDBX_CONFIG_H) восстанавливаем кавычки явно
            if t.startswith("-DMDBX_CONFIG_H="):
                k, _, v = t.partition("=")
                if v.startswith('"') and v.endswith('"'):
                    t = '%s=\\"%s\\"' % (k, v.strip('"'))
            defs.append(t)
        command = " ".join([
            "c++",
            "-DMDBX_BUILD_TEST=1", "-DMDBX_BUILD_CXX=1",
        ] + defs + base_includes + ["-I%s" % REPO_ROOT])
        out.append((prefix.rstrip("/") or domain,
                    {"file": abspath, "directory": REPO_ROOT,
                     "command": command}))
    return out


def link_symbols_to_regions(symbols: dict, regions: list) -> dict:
    """Привязывает символы к покрывающим их #if-регионам.

    Для каждого символа ищет самые внутренние регионы того же файла,
    чей диапазон [l0, l1] содержит диапазон символа. Результат — новый
    dict символов с полем `regions`: список id регионов (от внешнего
    к внутреннему). Символы вне условной компиляции получают пустой
    список (явное отличие от отсутствия данных).
    """
    from bisect import bisect_left, bisect_right
    by_file = {}
    for r in regions:
        by_file.setdefault(r["file"], []).append(r)
    for flist in by_file.values():
        flist.sort(key=lambda r: (r["l0"], -(r["l1"] or 0)))

    out = dict(symbols)
    for key, sym in symbols.items():
        bodies = [sym]
        if isinstance(sym, dict) and sym.get("implementations"):
            bodies = sym["implementations"]
        changed = False
        new_bodies = []
        for body in bodies:
            file = body.get("file")
            l0, l1 = body.get("l0", 0), body.get("l1", 0)
            # декларации без корректного диапазона (l1==0/l1<l0) не связываем:
            # регион им не подобрать достоверно
            if not file or not l0 or l1 < l0:
                new_bodies.append(body)
                continue
            flist = by_file.get(file, [])
            # быстрый отсев: бинарный поиск по l0
            starts = [r["l0"] for r in flist]
            i = bisect_right(starts, l0) - 1
            cover = []
            while i >= 0:
                r = flist[i]
                rl1 = r.get("l1") or l1
                if r["l0"] <= l0 and (rl1 or l1) >= l1:
                    cover.append(r["id"])
                i -= 1
            cover.reverse()
            if cover:
                body = dict(body)
                body["regions"] = cover
                changed = True
            new_bodies.append(body)
        if changed:
            sym = dict(sym)
            if sym.get("implementations"):
                sym["implementations"] = new_bodies
            else:
                sym.update(new_bodies[0])
            out[key] = sym
    return out


def annotate_configs(symbols: dict, regions: list) -> dict:
    """Добавляет символам фаcет `configs` по их #if-регионам.

    Извлекает платформенные/опционные условия покрывающих регионов:
      - IS_WINDOWS/_WIN32/_WIN64 → конфигурация "win32";
      - __linux__/__gnu_linux__ → "linux";
      - __APPLE__/__MACH__ → "macos";
      - MDBX_ENABLE_* / прочие опции → "option:<name>".
    Символы без регионов получают configs=["all"] (видны во всех
    конфигурациях); вне условной компиляции — явный маркер.

    Примечание: тела функций из веток, неактивных в текущей конфигурации
    (например lck-windows.c на Linux), в AST отсутствуют — их полнота
    достигается многоконфигурационным сканированием (см. docs).
    """
    out = dict(symbols)
    for key, sym in symbols.items():
        if sym.get("implementations"):
            # многоконфигурационный символ: общий фаcет = union фаcетов
            # тел; каждое тело уже аннотировано в link_symbols_to_regions.
            cfg = set()
            for impl in sym["implementations"]:
                icfg = _configs_from_regions(impl.get("regions") or [], regions)
                cfg |= icfg
                if not icfg:
                    cfg.add("all")
            if not cfg:
                cfg.add("all")
            sym = dict(sym)
            sym["configs"] = sorted(cfg)
            out[key] = sym
            continue
        rids = sym.get("regions")
        if not rids:
            # нет привязки к регионам → видим везде
            sym = dict(sym)
            sym["configs"] = ["all"]
            out[key] = sym
            continue
        cfg = _configs_from_regions(rids, regions)
        if not cfg:
            cfg.add("all")
        sym = dict(sym)
        sym["configs"] = sorted(cfg)
        out[key] = sym
    return out


def _configs_from_regions(rids: list, regions: list) -> set:
    """Извлекает конфигурации (win32/linux/macos/option:*) из списка регионов."""
    cfg = set()
    for rid in rids:
        reg = next((r for r in regions if r["id"] == rid), None)
        if not reg:
            continue
        cond = reg["cond"]
        negated = cond.lstrip().startswith("!")
        if not negated and any(t in cond for t in (
                "IS_WINDOWS", "_WIN32", "_WIN64", "__WINDOWS__",
                "_WINDOWS")):
            cfg.add("win32")
        if negated and any(t in cond for t in (
                "IS_WINDOWS", "_WIN32", "_WIN64", "__WINDOWS__",
                "_WINDOWS")):
            cfg.add("linux")
        if any(t in cond for t in ("__linux__", "__gnu_linux__")):
            cfg.add("linux")
        if any(t in cond for t in ("__APPLE__", "__MACH__")):
            cfg.add("macos")
        for m in re.finditer(r'\b(MDBX_[A-Z0-9_]+)\b', cond):
            cfg.add("option:%s" % m.group(1))
        for m in re.finditer(r'\bdefined\(([A-Z0-9_]+)\)', cond):
            name = m.group(1)
            if name not in ("IS_WINDOWS", "_WIN32", "_WIN64",
                            "__WINDOWS__", "_WINDOWS") and not name.startswith("__"):
                cfg.add("option:%s" % name)
    return cfg


def scan_configuration(clang, cc, tus, cfg: ScanConfig,
                       extra_symbols: dict = None) -> dict:
    """Сканирует один конфиг: символы (канонизированные), рёбра, ошибки.

    extra_symbols — внешние символы (например библиотечные), участвующие в
    резолве callee (для тестовых доменов: тест → функция библиотеки).
    """
    col = Collector()
    errors = {}
    t0 = time.time()
    for name, entry in tus:
        _, ast, err = run_tu(clang, name, entry, cfg)
        if err or ast is None:
            errors[name] = err or "empty AST"
            continue
        col.default_file = entry["file"]
        col.tu_dir = entry["directory"]
        col._loc_file = None
        walk(ast, col)
        scan_macros(clang, name, entry, col, cfg)
        print("  %-9s %s  (%ds)" % (name, os.path.relpath(entry["file"], REPO_ROOT),
                                    int(time.time() - t0)), file=sys.stderr)

    finalize_blocks(col)
    raw_symbols = {**col.functions, **col.types, **col.macros}
    symbols, key_map = canonicalize_symbols(raw_symbols)
    col.edges = [(key_map.get(caller, caller), name) for caller, name in col.edges]
    resolve_syms = dict(symbols)
    if extra_symbols:
        resolve_syms.update(extra_symbols)
    edges = resolve_edges(col, resolve_syms)
    return {"config": cfg.name, "symbols": symbols, "edges": edges,
            "errors": errors}


def _body_fingerprint(sym: dict) -> tuple:
    return (sym.get("file"), sym.get("l0"), sym.get("l1"))


def _make_impl(sym: dict, cfg_name: str) -> dict:
    keep = ("config", "file", "module", "l0", "l1", "blocks",
            "regions", "static", "is_definition", "configs", "extra_defs")
    impl = {k: sym.get(k) for k in keep if k in sym}
    impl["config"] = cfg_name
    return impl


def merge_configs(scans: list) -> tuple:
    """Сливает per-config сканы в один словарь символов.

    Правила (см. docs/REFACTORING-MAP.md, решение владельца + Алисы):
      - символ, увиденный в одном конфиге, остаётся плоским;
      - символ с телами в нескольких конфигах: общие поля наверху
        (kind/name/signature/cpp_status) + `implementations` — список
        [{config, file, module, l0, l1, blocks, regions, configs}];
      - идентичное тело в нескольких конфигах (та же file+l0+l1) —
        НЕ дублируется в implementations, помечается только `in_configs`.

    Возвращает (merged_symbols, merged_edges).
    """
    merged = {}
    for scan in scans:
        cfg_name = scan["config"]
        for key, sym in scan["symbols"].items():
            prev = merged.get(key)
            if prev is None:
                s = dict(sym)
                s["in_configs"] = [cfg_name]
                s["_fps"] = {_body_fingerprint(sym)}
                merged[key] = s
                continue
            prev["in_configs"].append(cfg_name)
            fp = _body_fingerprint(sym)
            if fp in prev["_fps"]:
                continue
            prev["_fps"].add(fp)
            impls = prev.setdefault("implementations", [])
            if not impls:
                impls.append(_make_impl(prev, prev["in_configs"][0]))
                for k in ("file", "module", "l0", "l1", "blocks",
                          "regions", "static", "is_definition"):
                    prev.pop(k, None)
            impls.append(_make_impl(sym, cfg_name))

    edges_seen, edges = set(), []
    for scan in scans:
        for e in scan["edges"]:
            ek = (e.get("caller"), e.get("callee"), e.get("kind"),
                  bool(e.get("resolved")))
            if ek not in edges_seen:
                edges_seen.add(ek)
                edges.append(e)

    out = {}
    for key, s in merged.items():
        s.pop("_fps", None)
        s["in_configs"] = sorted(s["in_configs"])
        if "implementations" in s:
            impls = s["implementations"]
            cfg_union = sorted({c for i in impls for c in i.get("configs", [])})
            for i in impls:
                i.pop("configs", None)
            if cfg_union:
                s["configs"] = cfg_union
        out[key] = s
    return out, edges


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=os.path.join(REPO_ROOT, "_build-scan", "compile_commands.json"))
    ap.add_argument("--out", default=os.path.join(TOOLS_DIR, "artifacts", "refactoring-map.json"))
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--config", default="linux", help="имя конфигурации")
    ap.add_argument("--target", default=None, help="clang --target (кросс-конфиг)")
    ap.add_argument("--defines", default=None, help="доп. -D макросы через пробел")
    ap.add_argument("--includes", default=None, help="доп. -I каталоги через пробел")
    ap.add_argument("--extra-flags", default=None, help="доп. флаги clang")
    ap.add_argument("--tests", choices=TEST_DOMAIN_NAMES, default=None,
                    help="сканировать тестовый домен (ut/issues/framework)")
    ap.add_argument("--merge", default=None, nargs="+",
                    metavar="ARTIFACT",
                    help="режим merge: списки per-config артефактов (JSON) -> --out")
    args = ap.parse_args()

    if args.merge:
        scans = []
        for path in args.merge:
            with open(path) as f:
                art = json.load(f)
            scans.append({"config": art["config"], "symbols": art["symbols"],
                          "edges": art["edges"]})
        symbols, edges = merge_configs(scans)
        regions = build_regions(collect_source_files())
        counts = {
            "functions": sum(1 for v in symbols.values() if v["kind"] == "function"),
            "types": sum(1 for v in symbols.values()
                         if v["kind"] in ("record", "cxxrecord", "enum", "typedef")),
            "macros": sum(1 for v in symbols.values() if v["kind"] == "macro"),
            "blocks": _count_blocks(symbols),
            "edges": len(edges),
            "unresolved_edges": sum(1 for e in edges if not e["resolved"]),
            "regions": len(regions),
        }
        artifact = {
            "platform": os.uname().sysname.lower() + "-" + os.uname().machine,
            "configs": sorted({s["config"] for s in scans}),
            "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "tus": [],
            "errors": {},
            "counts": counts,
            "symbols": symbols,
            "edges": edges,
            "regions": regions,
        }
        artifact["symbols"] = link_symbols_to_regions(artifact["symbols"], regions)
        artifact["symbols"] = annotate_configs(artifact["symbols"], regions)
        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        with open(args.out, "w") as f:
            json.dump(artifact, f, ensure_ascii=False, indent=1)
        print(json.dumps(artifact["counts"], indent=1))
        print("written:", args.out)
        return

    cc = json.load(open(args.cc))
    tus = select_tus(cc)
    if args.tests:
        tus = select_test_tus(args.tests)
        cfg = ScanConfig(name="tests:%s" % args.tests,
                         extra_flags="-Wno-implicit-function-declaration",
                         no_detailed_preprocessing=True)
        # библиотечные символы для резолва тест→lib (coverage seed)
        lib_artifact = None
        lib_path = os.path.join(TOOLS_DIR, "artifacts", "refactoring-map.json")
        if os.path.exists(lib_path):
            with open(lib_path) as f:
                lib_artifact = json.load(f)
        extra = (lib_artifact or {}).get("symbols")
    else:
        cfg = ScanConfig(name=args.config, target=args.target,
                         defines=args.defines, includes=args.includes,
                         extra_flags=args.extra_flags)
        extra = None
    scan = scan_configuration(args.clang, cc, tus, cfg, extra_symbols=extra)
    if args.tests:
        regions = build_regions(collect_source_files(include_tests=True))
    else:
        regions = build_regions(collect_source_files())
    symbols = scan["symbols"]
    edges = scan["edges"]
    artifact = {
        "platform": os.uname().sysname.lower() + "-" + os.uname().machine,
        "config": cfg.name,
        "build_config": os.path.basename(os.path.dirname(args.cc)),
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "tus": [os.path.relpath(e["file"], REPO_ROOT) for _, e in tus],
        "errors": scan["errors"],
        "counts": {
            "functions": sum(1 for v in symbols.values() if v["kind"] == "function"),
            "types": sum(1 for v in symbols.values()
                         if v["kind"] in ("record", "cxxrecord", "enum", "typedef")),
            "macros": sum(1 for v in symbols.values() if v["kind"] == "macro"),
            "blocks": _count_blocks(symbols),
            "edges": len(edges),
            "unresolved_edges": sum(1 for e in edges if not e["resolved"]),
            "regions": len(regions),
        },
        "symbols": symbols,
        "edges": edges,
        "regions": regions,
    }
    artifact["symbols"] = link_symbols_to_regions(artifact["symbols"], regions)
    artifact["symbols"] = annotate_configs(artifact["symbols"], regions)
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        json.dump(artifact, f, ensure_ascii=False, indent=1)
    print(json.dumps(artifact["counts"], indent=1))
    print("written:", args.out)


def _count_blocks(symbols: dict) -> int:
    total = 0
    for v in symbols.values():
        if v["kind"] != "function":
            continue
        if "implementations" in v:
            total += sum(len(i.get("blocks", [])) for i in v["implementations"])
        else:
            total += len(v.get("blocks", []))
    return total


if __name__ == "__main__":
    main()