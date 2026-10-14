#!/usr/bin/env python3
"""api_coverage.py — coverage of the libmdbx public API by the textbook.

Inventory sources:
  1. refactoring-map (scan_symbols.py output, loaded into MCP memory):
       - public C functions:  fn:mdbx_*  (no @module suffix)
       - public C types:      type:mdbx:* (record/enum/typedef from mdbx.h)
       - public C++ methods:  fn:mdbx::<Class>::<method>
  2. direct header parsing (closes the scanner gap — enum *enumerators* are
     not indexed by scan_symbols):
       - mdbx.h:        typedef enum {...} blocks + #define MDBX_* constants
       - mdbx.h++ and mdbx++/decl_*.h++: enum class enumerators (C++ API)

Corpus: docs/textbook/ru/*.md (only the Russian volumes, per decision).

Matching:
  - C identifiers (mdbx_*, MDBX_*): exact word-boundary occurrence;
  - C++ methods/constants: short name (last segment after ::); generic short
    names (get/set/put/...) are reported separately as "possible false hits".

Output: summary to stdout; full markdown report written to --out.
"""

import argparse
import json
import os
import re
import sys

# ---------------------------------------------------------------------------
# Inventory: C public functions / types from the refactoring-map
# ---------------------------------------------------------------------------

C_FN_RE = re.compile(r"^fn:mdbx_[A-Za-z][A-Za-z0-9_]+$")

# Clearly-internal helpers that happen to carry the mdbx_ prefix (library
# bootstrap, Windows ANSI/OEM conversion, SEH trampolines).
C_INTERNAL_EXCLUDE = frozenset({
    "mdbx_global_constructor", "mdbx_global_destructor",
    "mdbx_simple_SEH", "mdbx_strerror_ANSI2OEM",
    "mdbx_strerror_r_ANSI2OEM", "mdbx_RegGetValue",
})

# Public C++ classes (allowlist). Everything under these is counted.
CPP_PUBLIC_CLASSES = frozenset({
    "env", "env_managed", "txn", "txn_managed",
    "cursor", "cursor_managed", "map_handle",
    "slice", "buffer", "error", "exception",
    "multivalue", "value_result", "pair_result", "pair",
    "cache_entry",
    # typed exceptions
    "bad_map_id", "bad_transaction", "bad_value_size",
    "dangling_map_id", "db_corrupted", "db_full", "db_invalid",
    "db_too_large", "db_unable_extend", "db_version_mismatch",
    "db_wanna_write_for_recovery", "duplicated_lck_file",
    "fatal", "incompatible_operation", "internal_page_full",
    "internal_problem", "key_exists", "key_mismatch",
    "laggard_reader", "max_maps_reached", "max_readers_reached",
    "mvcc_retarded", "no_data", "not_found",
    "operation_not_permitted", "permission_denied_or_not_writeable",
    "reader_slot_busy", "remote_media", "something_busy",
    "thread_mismatch", "transaction_full", "transaction_ousted",
    "transaction_overlapping",
})

# Internal detail namespaces/classes that must NOT be counted as API.
CPP_INTERNAL = frozenset({
    "allocation_aware_details", "b58_buffer", "default_capacity_policy",
    "from_base58", "from_base64", "from_hex",
    "to_base58", "to_base64", "to_hex",
    "operator<<", "gc_iter_thunk", "defrag_thunk",
    "reader_visitor_thunk", "tables_enum_thunk", "exception_thunk",
    "slice mdbx",
})


def load_map(path):
    with open(path) as f:
        return json.load(f)["symbols"]


def inventory_from_map(symbols):
    c_functions = []
    c_types = []          # (kind, name) from module mdbx (mdbx.h)
    cpp_methods = []      # (class_name, short_name, full_key)
    for key, v in symbols.items():
        kind = v["kind"]
        if kind == "function":
            if C_FN_RE.match(key):
                name = key[3:]
                if name not in C_INTERNAL_EXCLUDE:
                    c_functions.append(name)
                continue
            if key.startswith("fn:mdbx::") and "@" not in key:
                cls = _cpp_class_of(key)
                if cls and cls in CPP_PUBLIC_CLASSES:
                    short = _method_short(key, cls)
                    if short:
                        cpp_methods.append((cls, short, key))
        elif kind in ("record", "enum", "typedef") and key.startswith("type:mdbx:"):
            c_types.append((kind, key[len("type:mdbx:"):]))
    c_functions = sorted(set(c_functions))
    c_types = sorted(set(c_types))
    cpp_methods = sorted(set(cpp_methods))
    return c_functions, c_types, cpp_methods


def _cpp_class_of(key):
    rest = key[len("fn:mdbx::"):]
    seg = rest.split("::", 1)[0]
    if "<" in seg:
        return None
    return seg


_HASH_SUFFIX = re.compile(r"#\w+$")


def _method_short(key, cls):
    rest = key[len("fn:mdbx::"):]
    body = rest.split("::", 1)[1] if "::" in rest else rest
    if "<" in body or "(" in body:
        return None
    short = _HASH_SUFFIX.sub("", body.rstrip())
    if short.startswith("~") or short.startswith("operator"):
        return None
    if short == cls:
        return None  # constructor/destructor-like
    return short


# ---------------------------------------------------------------------------
# Inventory: enum constants / defines from mdbx.h
# ---------------------------------------------------------------------------

_MDBX_DEFINE = re.compile(r"^#\s*define\s+(MDBX_[A-Za-z0-9_]+)", re.M)
_ENUM_BLOCK = re.compile(r"typedef\s+enum\s+(\w+)\s*\{")


def parse_mdbx_h(path):
    """Returns (enumerators, defines).

    enumerators: list of (owning_enum, name) — only real enumerators (names
                 before `=`/`,`/`}`), not initializer values;
    defines:     list of names (MDBX_* constants defined via #define).
    """
    text = open(path).read()
    defines = sorted(set(_MDBX_DEFINE.findall(text)))
    enumerators = []
    for m in _ENUM_BLOCK.finditer(text):
        enum_name = m.group(1)
        start = m.end()
        depth = 1
        i = start
        while i < len(text) and depth:
            ch = text[i]
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            i += 1
        body = text[start:i - 1]
        toks = re.findall(r"\{|\}|[:=,]|[A-Za-z_]\w*", body)
        in_values = False
        for t in toks:
            if t == "=":
                in_values = True
            elif t == ",":
                in_values = False
            elif t in ("{", "}", ":"):
                continue
            elif not in_values and t.startswith("MDBX_"):
                enumerators.append((enum_name, t))
    return enumerators, defines


# ---------------------------------------------------------------------------
# Inventory: C++ enum classes from mdbx.h++ / mdbx++/decl_*.h++
# ---------------------------------------------------------------------------

_CPP_CLASS_NOISE = frozenset({"final", "public", "private", "protected"})


def _strip_cpp_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"(?m)^[ \t]*#.*$", " ", text)
    return text


# Display grouping for C++ enums: which logical namespace the header belongs to.
_CPP_SCOPE_HINT = {
    "begin.h++": "mdbx",
    "decl_core.h++": "mdbx",
    "decl_slice.h++": "mdbx",
    "decl_transcoders.h++": "mdbx",
    "decl_env.h++": "env",
    "decl_txn.h++": "txn",
    "decl_cursor.h++": "cursor",
    "decl_buffer.h++": "buffer",
    "decl_exceptions.h++": "exception",
}


def _cpp_scope_hint(path):
    base = os.path.basename(path)
    return _CPP_SCOPE_HINT.get(base, "mdbx")


def parse_cpp_enums(paths):
    """Returns list of (scope, enum_name, [enumerators]).

    A clean token walk over comment-free text: tracks enclosing class/struct
    names and collects enumerators strictly inside each `enum class {...}`
    brace span (so Doxygen prose cannot leak into the inventory).
    """
    out = []
    ident = re.compile(r"[A-Za-z_]\w*")
    for path in paths:
        try:
            text = _strip_cpp_comments(open(path).read())
        except OSError:
            continue
        scope_hint = _cpp_scope_hint(path)
        toks = re.findall(r"\{|\}|[:=,]|[A-Za-z_]\w*", text)
        stack = []   # ('open', None) | ('class', name) | ('enum', name)
        i, n = 0, len(toks)
        while i < n:
            t = toks[i]
            if t == "{":
                stack.append(("open", None))
                i += 1
                continue
            if t == "}":
                if stack and stack[-1][0] == "open":
                    stack.pop()
                elif stack and stack[-1][0] in ("class", "enum"):
                    stack.pop()
                i += 1
                continue
            if t in ("class", "struct"):
                j = i + 1
                name = None
                while j < n and toks[j] not in ("{", ":"):
                    cand = toks[j]
                    if cand not in _CPP_CLASS_NOISE and cand.islower():
                        name = cand
                    j += 1
                if name:
                    stack.append(("class", name))
                i = j
                continue
            if t == "enum":
                j = i + 1
                if j < n and toks[j] == "class":
                    j += 1
                    if j < n and ident.match(toks[j]):
                        enum_name = toks[j]
                        k = j + 1
                        while k < n and toks[k] not in ("{", "}"):
                            k += 1
                        if k < n and toks[k] == "{":
                            items, depth = [], 1
                            k += 1
                            in_values = False
                            while k < n and depth:
                                tk = toks[k]
                                if tk == "{":
                                    depth += 1
                                elif tk == "}":
                                    depth -= 1
                                    if depth == 0:
                                        break
                                elif tk == "=":
                                    in_values = True
                                elif tk == ",":
                                    in_values = False
                                elif not in_values and ident.match(tk):
                                    items.append(tk)
                                k += 1
                            scope = scope_hint
                            out.append((scope, enum_name,
                                        sorted(set(items))))
                            i = k + 1
                            continue
                i = j
                continue
            i += 1
    return out


# ---------------------------------------------------------------------------
# Corpus + matching
# ---------------------------------------------------------------------------

_CORPUS_IDENT = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")


def build_corpus(dir_path):
    parts = []
    for fn in sorted(os.listdir(dir_path)):
        if fn.endswith(".md"):
            parts.append(open(os.path.join(dir_path, fn)).read())
    return "\n".join(parts)


def word_pattern(name):
    return re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) +
                      r"(?![A-Za-z0-9_])")


def check_coverage(corpus, names, cache=None):
    if cache is None:
        cache = {}
    out = {}
    for name in names:
        if name not in cache:
            cache[name] = bool(word_pattern(name).search(corpus))
        out[name] = cache[name]
    return out


# Generic C++ short names that are likely to match prose spuriously.
GENERIC_SHORT = frozenset({
    "get", "set", "put", "del", "open", "close", "begin", "end", "next",
    "prev", "first", "last", "count", "size", "empty", "clone", "move",
    "swap", "clear", "read", "write", "copy", "bind", "commit", "abort",
    "mode", "value", "data", "key", "info", "stat", "name", "path",
    "result", "status", "error", "time", "length", "c_str", "compare",
})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default=(
        "/sourcecraft/workspace/nook-pool-1/skynet/"
        "MCP-shared-graph-memory/tools/artifacts/refactoring-map.json"))
    ap.add_argument("--mdbx-h", default="mdbx.h")
    ap.add_argument("--cpp-dir", default="mdbx++")
    ap.add_argument("--corpus-dir", default="docs/textbook/ru")
    ap.add_argument("--out", default=None,
                    help="markdown report path "
                         "(default: nook-owner-dont-touch/for-landing/"
                         "review-textbook-api-coverage.md)")
    args = ap.parse_args()

    base = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(base))  # skill-tree
    mdbx_h = args.mdbx_h if os.path.isabs(args.mdbx_h) else \
        os.path.join(repo, args.mdbx_h)
    cpp_dir = args.cpp_dir if os.path.isabs(args.cpp_dir) else \
        os.path.join(repo, args.cpp_dir)
    corpus_dir = args.corpus_dir if os.path.isabs(args.corpus_dir) else \
        os.path.join(repo, args.corpus_dir)

    symbols = load_map(args.map)
    c_functions, c_types, cpp_methods = inventory_from_map(symbols)

    enum_consts, defines = parse_mdbx_h(mdbx_h)
    cpp_enum_files = sorted(
        [os.path.join(cpp_dir, f) for f in os.listdir(cpp_dir)
         if f.endswith((".h++", ".h"))])
    cpp_enums = parse_cpp_enums(cpp_enum_files + [os.path.join(repo, "mdbx.h++")])

    corpus = build_corpus(corpus_dir)
    cache = {}

    c_fn_cov = check_coverage(corpus, c_functions, cache)
    c_type_names = [n for _, n in c_types]
    c_type_cov = check_coverage(corpus, c_type_names, cache)
    c_enum_names = sorted({n for _, n in enum_consts})
    c_enum_cov = check_coverage(corpus, c_enum_names, cache)
    c_def_cov = check_coverage(corpus, defines, cache)

    # C++ methods by short name
    cpp_by_class = {}
    for cls, short, full in cpp_methods:
        cpp_by_class.setdefault(cls, []).append((short, full))
    cpp_short_names = sorted({s for cls in cpp_by_class for s, _ in cpp_by_class[cls]})
    cpp_short_cov = check_coverage(corpus, cpp_short_names, cache)
    generic = sorted(n for n in cpp_short_names
                     if n.lower() in GENERIC_SHORT or n in GENERIC_SHORT)

    # C++ enum enumerators
    cpp_enum_names = sorted({e for _, _, es in cpp_enums for e in es})
    cpp_enum_cov = check_coverage(corpus, cpp_enum_names, cache)

    # ---- summary -------------------------------------------------------
    def summarize(names, cov, label):
        total = len(names)
        covered = sum(1 for n in names if cov[n])
        if total:
            print(f"{label:32s} {covered:5d}/{total:<5d}  "
                  f"({100.0*covered/total:5.1f}%)")
        else:
            print(f"{label}: 0")
        return covered, total

    print("== API coverage by RU textbook ==")
    cf_c, cf_t = summarize(c_functions, c_fn_cov, "C functions")
    ct_c, ct_t = summarize(c_type_names, c_type_cov, "C struct types")
    ce_c, ce_t = summarize(c_enum_names, c_enum_cov, "C enum constants")
    cd_c, cd_t = summarize(defines, c_def_cov, "C #define constants")
    cm_c, cm_t = summarize(cpp_short_names, cpp_short_cov, "C++ methods (short)")
    cpe_c, cpe_t = summarize(cpp_enum_names, cpp_enum_cov, "C++ enum constants")

    # ---- write report ---------------------------------------------------
    out = args.out
    if out is None:
        out = os.path.join(
            os.path.dirname(repo), "nook-owner-dont-touch", "for-landing",
            "review-textbook-api-coverage.md")

    def table(names, cov, title, per_line=3):
        rows = []
        for i in range(0, len(names), per_line):
            cells = []
            for n in names[i:i + per_line]:
                mark = "" if cov[n] else " ***** **"
                cells.append("`%s`" % n)
            rows.append("| " + " | ".join(cells) + " |")
        return "\n".join(rows)

    def uncovered_list(names, cov):
        return [n for n in names if not cov[n]]

    lines = []
    lines.append("# Отчёт: покрытие API libmdbx учебником")
    lines.append("")
    lines.append("Анализ: какие элементы публичного API libmdbx **не фигурируют** в учебнике"
                 " (корпус — только русские тома `docs/textbook/ru/`).")
    lines.append("")
    lines.append("## Методология и источники")
    lines.append("")
    lines.append("- **Инвентарь функций и типов:** refactoring-map (`scan_symbols.py`),"
                 " артефакт от 2026-09-29 (`refactoring-map.json`, counts: functions 4448,"
                 " types 406, macros 573); из него взяты публичные `fn:mdbx_*` и"
                 " `type:mdbx:*` (структуры/enum-типы/typedef из `mdbx.h`), а также методы"
                 " публичных C++-классов `fn:mdbx::<Class>::*`. Исключены внутренние хелперы"
                 " (bootstrap/SEH/ANSI2OEM: `mdbx_global_constructor`, `mdbx_simple_SEH`,"
                 " `mdbx_strerror_*_ANSI2OEM`, …), детали реализации C++ (`*_details`,"
                 " шаблонные `buffer<std…>`, конвертеры `from_base*/to_base*`, `*thunk`,"
                 " `operator*`).")
    lines.append("- **Enum-константы:** прямым парсингом заголовков `mdbx.h` и `mdbx++/decl_*.h++`,"
                 " потому что сканер refactoring-map индексирует только имена enum-типов и `#define`,"
                 " но не перечислители `typedef enum {…}`.")
    lines.append("- **Корпус:** `docs/textbook/ru/*.md` (7 файлов), включая фрагменты кода в тексте.")
    lines.append("- **Матчинг:** точное вхождение идентификатора по границе слова. Для C++-методов"
                 " матчится краткое имя (последний сегмент после `::`); короткие общие имена"
                 " (`get`, `set`, `put`, `mode`, …) могут давать ложные срабатывания и вынесены"
                 " отдельно.")
    lines.append("")
    lines.append("## Сводка")
    lines.append("")
    lines.append("| Категория | Покрыто | Всего | Доля |")
    lines.append("| --- | ---: | ---: | ---: |")
    def pct(c, t):
        return f"{100.0*c/t:.1f}%" if t else "—"

    lines.append(f"| C-функции | {cf_c} | {cf_t} | {pct(cf_c, cf_t)} |")
    lines.append(f"| C-структурные типы | {ct_c} | {ct_t} | {pct(ct_c, ct_t)} |")
    lines.append(f"| C-enum-константы (перечислители) | {ce_c} | {ce_t} | {pct(ce_c, ce_t)} |")
    lines.append(f"| C-#define-константы | {cd_c} | {cd_t} | {pct(cd_c, cd_t)} |")
    lines.append(f"| C++-методы (краткое имя) | {cm_c} | {cm_t} | {pct(cm_c, cm_t)} |")
    lines.append(f"| C++-константы (enum class) | {cpe_c} | {cpe_t} | {pct(cpe_c, cpe_t)} |")
    lines.append("")
    lines.append("## Не покрыто учебником")
    lines.append("")
    if cf_c < cf_t:
        lines.append("### C-функции")
        lines.append("")
        lines.append(table(uncovered_list(c_functions, c_fn_cov), c_fn_cov, ""))
        lines.append("")
    if ct_c < ct_t:
        lines.append("### C-структурные типы")
        lines.append("")
        lines.append(table(uncovered_list(c_type_names, c_type_cov), c_type_cov, "", per_line=2))
        lines.append("")
    if ce_c < ce_t:
        lines.append("### C-enum-константы (не встречены в тексте)")
        lines.append("")
        by_enum = {}
        for enum_name, n in enum_consts:
            if not c_enum_cov[n]:
                by_enum.setdefault(enum_name, set()).add(n)
        for enum_name in sorted(by_enum):
            lines.append(f"- `{enum_name}`: " +
                         ", ".join("`%s`" % n for n in sorted(by_enum[enum_name])))
        lines.append("")
    if cd_c < cd_t:
        lines.append("### C-#define-константы (не встречены в тексте)")
        lines.append("")
        lines.append(table(uncovered_list(defines, c_def_cov), c_def_cov, ""))
        lines.append("")
        lines.append("> Все `MDBX_*`-#define — макросы инфраструктуры компиляции/версионирования"
                     " (`MDBX_PURE_FUNCTION`, `MDBX_NORETURN`, `MDBX_LIKELY`,"
                     " `MDBX_VERSION_MAJOR/MINOR`, имена файлов БД/блокировок `MDBX_DATANAME`/"
                     "`MDBX_LOCKNAME` и т.п.). Их отсутствие в учебнике ожидаемо и не является"
                     " пробелом в описании API.")
        lines.append("")
    if cm_c < cm_t:
        unc = uncovered_list(cpp_short_names, cpp_short_cov)
        non_generic = [n for n in unc if n not in generic]
        lines.append("### C++-методы (краткое имя не встречено)")
        lines.append("")
        if non_generic:
            by_class = {}
            for cls, lst in cpp_by_class.items():
                seen = set()
                for s, full in lst:
                    if s in non_generic and s not in seen:
                        seen.add(s)
                        by_class.setdefault(cls, []).append(s)
            for cls in sorted(by_class):
                lines.append(f"- **`{cls}`**: " +
                             ", ".join("`%s`" % s for s in sorted(by_class[cls])))
        else:
            lines.append("(нет)")
        lines.append("")
        found_generic = sorted(n for n in generic if cpp_short_cov[n])
        if found_generic:
            lines.append("### C++-методы: короткие общие имена (найдены в тексте — возможны"
                         " ложные срабатывания)")
            lines.append("")
            lines.append(", ".join("`%s`" % n for n in found_generic))
            lines.append("")
    if cpe_c < cpe_t:
        lines.append("### C++-константы (enum class, не встречены в тексте)")
        lines.append("")
        for scope, enum_name, es in cpp_enums:
            missing = [e for e in es if not cpp_enum_cov[e]]
            if missing:
                where = f"{scope}::{enum_name}" if scope else enum_name
                lines.append(f"- `{where}`: " +
                             ", ".join("`%s`" % e for e in missing))
        lines.append("")
    lines.append("## Оговорки")
    lines.append("")
    lines.append("- Корпус — только русские тома; английская версия не учитывалась"
                 " (идентификаторы в ней те же, но по решению считаем RU).")
    lines.append("- `MDBX_*`-токены, встречающиеся в `mdbx.h` как идентификаторы типов/значений в"
                 " объявлениях (например, в сигнатурах), в инвентарь констант не входят — только"
                 " перечислители и `#define`.")
    lines.append("- Покрытие засчитывается по любому вхождению в текст томов (проза, таблицы,"
                 " фрагменты кода), включая ссылки `[`имя`](...)`.")
    lines.append("- Краткие имена C++-методов вида `get`/`set`/`put`/`mode` и т.п. почти всегда"
                 " «находятся» в прозе — это ожидаемые ложные срабатывания; значимо отсутствие"
                 " не-generic имён (список выше).")
    lines.append("- Enum-константы C API вне карты refactoring-map (пробел сканера: перечислители"
                 " не индексируются) — здесь получены парсингом `mdbx.h`.")
    lines.append("")

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        f.write("\n".join(lines))
    print("report written:", out)


if __name__ == "__main__":
    main()