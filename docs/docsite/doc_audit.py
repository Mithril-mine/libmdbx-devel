#!/usr/bin/env python3
"""doc_audit.py — published-docs integrity audit for the libmdbx docsite.

Unifies the two prior dev tools:

  1. `amalgamate_md.py` — published documents must be applicable to the
     amalgamated package: every repo file they reference in markdown links
     must exist there (or be explicitly allow-listed as a dev-only pointer);
     `<!-- dist-cutoff-begin/end -->` markers must be paired and are reported
     (they are stripped by `make dist` the same way as in C sources).

  2. `api_coverage.py` — coverage of the libmdbx public API by the RU
     textbook. Requires an optional refactoring-map artifact produced by
     `scan_symbols.py`; when absent the coverage report is skipped and the
     audit still succeeds (the dist-cutoff/refs check is the mandatory part).

Usage:
  python3 doc_audit.py                # audit docs/{ru,en}/ against package
                                      # (dist-cutoff + refs); API coverage
                                      # skipped unless refactoring-map found
  python3 doc_audit.py --map PATH     # also run API-coverage report

The refactoring-map default is `${MAP_DIR}/MCP-shared-graph-memory/tools/
artifacts/refactoring-map.json`.
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

BEGIN = "<!-- dist-cutoff-begin -->"
END = "<!-- dist-cutoff-end -->"
# dev-only docs/eng helpers that published pages may mention as pointers
# (they are NOT part of the amalgamated package but referencing them is a
# conscious editorial decision, not an accident)
ALLOW_DEV = {"docs/engineering/", "skynet/", "tests/", "CHANGELOG"}

MD_LINK = re.compile(r"\[[^\]]*\]\(([^)#\s]+)(?:#[^)]*)?\)")


def strip_dist_cutoff(text: str, origin: str = "") -> str:
    out, pos, cuts = [], 0, 0
    while True:
        b = text.find(BEGIN, pos)
        if b < 0:
            out.append(text[pos:])
            break
        e = text.find(END, b)
        if e < 0:
            raise ValueError(f"{origin}: unclosed {BEGIN}")
        out.append(text[pos:b])
        pos = e + len(END)
        cuts += 1
    if text.count(BEGIN) != text.count(END):
        raise ValueError(f"{origin}: unbalanced dist-cutoff markers")
    return "".join(out), cuts


def audit_amalgamated_refs(md_path: Path, package_root: Path):
    """Report markdown links pointing to files that do not exist in the
    amalgamated package. Only repo-relative links are checked; http(s) and
    pure anchors are skipped."""
    violations = []
    text = md_path.read_text(encoding="utf-8")
    for m in MD_LINK.finditer(text):
        target = m.group(1)
        if target.startswith(("http://", "https://", "mailto:")):
            continue
        if target.startswith("#"):
            continue
        # resolve relative to the doc location, then try to map into the repo
        rel = (md_path.parent / target).resolve()
        candidates = [rel, package_root / target]
        p2 = md_path.parents[2] if len(md_path.parents) > 2 else None
        if p2 and rel.is_relative_to(p2):
            candidates.append(package_root / rel.relative_to(p2))
        ok = any(c and c.exists() for c in candidates if c)
        if not ok:
            violations.append(f"{md_path.name}: {target}")
    return violations


PUBLISHED = frozenset({
    "architecture.md", "internals.md", "improvements.md",
})


def audit_amalgamated(pkg: Path) -> int:
    bad = 0
    for loc in ("ru", "en"):
        for md in sorted((pkg / "docs" / loc / "deep-dive").glob("*.md")):
            if md.name not in PUBLISHED:
                continue  # dev-only docs are not published; skip the audit
            try:
                _, cuts = strip_dist_cutoff(md.read_text(encoding="utf-8"), str(md))
            except ValueError as e:
                print(f"  MARKER FAIL: {e}")
                bad += 1
                continue
            if cuts:
                print(f"  {md.name}: {cuts} dist-cutoff regions")
            for v in audit_amalgamated_refs(md, pkg):
                print(f"  REF FAIL: {v}")
                bad += 1
    return bad


# ---------------------------------------------------------------------------
# API coverage by the RU textbook (port of docs/api_coverage.py)
# ---------------------------------------------------------------------------

C_FN_RE = re.compile(r"^fn:mdbx_[A-Za-z][A-Za-z0-9_]+$")
C_INTERNAL_EXCLUDE = frozenset({
    "mdbx_global_constructor", "mdbx_global_destructor",
    "mdbx_simple_SEH", "mdbx_strerror_ANSI2OEM",
    "mdbx_strerror_r_ANSI2OEM", "mdbx_RegGetValue",
})
CPP_PUBLIC_CLASSES = frozenset({
    "env", "env_managed", "txn", "txn_managed",
    "cursor", "cursor_managed", "map_handle",
    "slice", "buffer", "error", "exception",
    "multivalue", "value_result", "pair_result", "pair",
    "cache_entry",
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
CPP_INTERNAL = frozenset({
    "allocation_aware_details", "b58_buffer", "default_capacity_policy",
    "from_base58", "from_base64", "from_hex",
    "to_base58", "to_base64", "to_hex",
    "operator<<", "gc_iter_thunk", "defrag_thunk",
    "reader_visitor_thunk", "tables_enum_thunk", "exception_thunk",
    "slice mdbx",
})

_MDBX_DEFINE = re.compile(r"^#\s*define\s+(MDBX_[A-Za-z0-9_]+)", re.M)
_ENUM_BLOCK = re.compile(r"typedef\s+enum\s+(\w+)\s*\{")


def load_map(path):
    with open(path) as f:
        return json.load(f)["symbols"]


def inventory_from_map(symbols):
    c_functions, c_types, cpp_methods = [], [], []
    for key, v in symbols.items():
        kind = v["kind"]
        if kind == "function":
            if C_FN_RE.match(key):
                name = key[3:]
                if name not in C_INTERNAL_EXCLUDE:
                    c_functions.append(name)
                continue
            if key.startswith("fn:mdbx::") and "@" not in key:
                rest = key[len("fn:mdbx::"):]
                seg = rest.split("::", 1)[0]
                if seg in CPP_PUBLIC_CLASSES:
                    body = rest.split("::", 1)[1] if "::" in rest else rest
                    if "<" not in body and "(" not in body:
                        short = re.sub(r"#\w+$", "", body.rstrip())
                        if short and not short.startswith("~") and not short.startswith("operator") and short != seg:
                            cpp_methods.append((seg, short, key))
        elif kind in ("record", "enum", "typedef") and key.startswith("type:mdbx:"):
            c_types.append((kind, key[len("type:mdbx:"):]))
    return sorted(set(c_functions)), sorted(set(c_types)), sorted(set(cpp_methods))


def parse_mdbx_h(path):
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
_CPP_CLASS_NOISE = frozenset({"final", "public", "private", "protected"})


def _strip_cpp_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r"(?m)^[ \t]*#.*$", " ", text)
    return text


def parse_cpp_enums(paths):
    out = []
    ident = re.compile(r"[A-Za-z_]\w*")
    for path in paths:
        try:
            text = _strip_cpp_comments(open(path).read())
        except OSError:
            continue
        base = os.path.basename(path)
        scope_hint = _CPP_SCOPE_HINT.get(base, "mdbx")
        toks = re.findall(r"\{|\}|[:=,]|[A-Za-z_]\w*", text)
        stack = []
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
                            out.append((scope_hint, enum_name, sorted(set(items))))
                            i = k + 1
                            continue
                i = j
                continue
            i += 1
    return out


_CORPUS_IDENT = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")


def build_corpus(dir_path):
    parts = []
    if not dir_path.is_dir():
        return ""
    for fn in sorted(dir_path.glob("*.md")):
        parts.append(fn.read_text(encoding="utf-8"))
    return "\n".join(parts)


def word_pattern(name):
    return re.compile(r"(?<![A-Za-z0-9_])" + re.escape(name) + r"(?![A-Za-z0-9_])")


def check_coverage(corpus, names, cache=None):
    if cache is None:
        cache = {}
    out = {}
    for name in names:
        if name not in cache:
            cache[name] = bool(word_pattern(name).search(corpus))
        out[name] = cache[name]
    return out


GENERIC_SHORT = frozenset({
    "get", "set", "put", "del", "open", "close", "begin", "end", "next",
    "prev", "first", "last", "count", "size", "empty", "clone", "move",
    "swap", "clear", "read", "write", "copy", "bind", "commit", "abort",
    "mode", "value", "data", "key", "info", "stat", "name", "path",
    "result", "status", "error", "time", "length", "c_str", "compare",
})


def api_coverage_report(map_path: Path, repo: Path) -> int:
    mdbx_h = repo / "mdbx.h"
    cpp_dir = repo / "mdbx++"
    corpus_dir = repo / "docs" / "textbook" / "ru"
    corpus = build_corpus(corpus_dir)
    cache = {}
    try:
        symbols = load_map(map_path)
    except (OSError, ValueError, KeyError) as e:
        print(f"  API-COVERAGE SKIP: refactoring-map unavailable ({e})")
        return 0

    c_functions, c_types, cpp_methods = inventory_from_map(symbols)
    enum_consts, defines = parse_mdbx_h(mdbx_h)
    cpp_enum_files = [p for p in cpp_dir.glob("*.h++") if p.is_file()]
    cpp_enums = parse_cpp_enums(cpp_enum_files + [repo / "mdbx.h++"])

    c_fn_cov = check_coverage(corpus, c_functions, cache)
    c_type_names = [n for _, n in c_types]
    c_type_cov = check_coverage(corpus, c_type_names, cache)
    c_enum_names = sorted({n for _, n in enum_consts})
    c_enum_cov = check_coverage(corpus, c_enum_names, cache)
    c_def_cov = check_coverage(corpus, defines, cache)
    cpp_short_names = sorted({s for _, s, _ in cpp_methods})
    cpp_short_cov = check_coverage(corpus, cpp_short_names, cache)
    cpp_enum_names = sorted({e for _, _, es in cpp_enums for e in es})
    cpp_enum_cov = check_coverage(corpus, cpp_enum_names, cache)
    generic = sorted(n for n in cpp_short_names if n in GENERIC_SHORT)

    def summarize(names, cov, label):
        total = len(names)
        covered = sum(1 for n in names if cov[n])
        print(f"{label:32s} {covered:5d}/{total:<5d}  "
              f"({100.0*covered/total:5.1f}%)" if total else f"{label}: 0")
    print("== API coverage by RU textbook ==")
    summarize(c_functions, c_fn_cov, "C functions")
    summarize(c_type_names, c_type_cov, "C struct types")
    summarize(c_enum_names, c_enum_cov, "C enum constants")
    summarize(defines, c_def_cov, "C #define constants")
    summarize(cpp_short_names, cpp_short_cov, "C++ methods (short)")
    summarize(cpp_enum_names, cpp_enum_cov, "C++ enum constants")

    uncovered = [n for n in c_functions if not c_fn_cov[n]]
    if uncovered:
        print(f"  UNCOVERED C functions: {len(uncovered)} "
              f"(e.g. {', '.join(uncovered[:8])})")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--map", default=None,
                    help="refactoring-map.json path (default: look in the "
                         "neighbouring skynet tree)")
    ap.add_argument("--package", default=".",
                    help="package root (default: the current directory)")
    args = ap.parse_args()

    pkg = Path(args.package).resolve()
    repo = pkg
    bad = 0
    bad += audit_amalgamated(pkg)

    if args.map:
        map_path = Path(args.map)
        if not map_path.is_absolute():
            map_path = pkg / map_path
        if map_path.is_file():
            # in a non-amalgamated dev tree the headers live at the repo
            # root; in a dist-tree (no mdbx.h at root — only dist/mdbx.h)
            # skip the coverage part
            if (repo / "mdbx.h").is_file():
                bad += api_coverage_report(map_path, repo)
            else:
                print("  API-COVERAGE SKIP: mdbx.h not found at the package root")
        else:
            print(f"  API-COVERAGE SKIP: {map_path} not found")
    else:
        # optional: auto-detect the neighbouring refactoring-map artifact
        default = pkg.parent / "skynet" / "MCP-shared-graph-memory" / "tools" / "artifacts" / "refactoring-map.json"
        if default.is_file() and (repo / "mdbx.h").is_file():
            bad += api_coverage_report(default, repo)

    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
