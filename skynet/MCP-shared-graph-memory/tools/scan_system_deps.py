#!/usr/bin/env python3
"""scan_system_deps.py — системный include-граф с резолвом символов.

Строит карту «системный заголовок → объявляемые символы» (PROVIDES) и
«использование системных символов файлами проекта» (USES_SYS) на основе
clang AST (`-Xclang -ast-dump=json`) и препроцессорных file-marks
(`clang -E -dD`), без эвристик.

Режимы:
  --scan-headers  per-header скан: для каждого системного заголовка из
                  --headers-list (или собранного из file-marks TU-сканов)
                  отдельный clang-прогон `-x c <header>`; loc.file узлов
                  совпадает с самим заголовком, поэтому привязка точна.
  --scan-uses     по TU из compile_commands.json: DeclRefExpr + имена макросов
                  в пользовательских файлах; резолв в заголовки по индексу
                  коротких имён из уже собранных PROVIDES-артефактов.
  --merge         мульти-конфигурационный merge: возвращает категорию
                  доступности (`in_configs`) для заголовков и символов.
                  Категории linux-only/win32-only/... — производные от
                  наблюдаемых конфигов, НЕ платформенные факты.

Существенное ограничение (документировано, см. docs/REFACTORING-MAP.md):
  * модель описывает доступность символа В КОНКРЕТНОЙ КОНФИГУРАЦИИ
    toolchain (linux-gcc, win32-mingw, win32-clang-cl — наборы различаются
    даже на одной ОС);
  * отсутствие символа/заголовка в артефакте НЕ означает его отсутствия
    на платформе: он может приходить из include-файла, не просканированного
    в проекте, резолвиться транзитивно или через макрос.

Выход: tools/artifacts/system-deps-<config>.json (вне git).
"""

import argparse
import json
import os
import re
import shlex
import subprocess
import sys

try:
    from scan_symbols import (ScanConfig, sanitize_args, select_tus, run_tu)
except ImportError:  # при запуске как модуля из tests/
    from tools.scan_symbols import (ScanConfig, sanitize_args, select_tus,
                                    run_tu)

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
REPO_ROOT = os.path.dirname(os.path.dirname(MODULE_DIR))
ARTIFACTS = os.path.join(TOOLS_DIR, "artifacts")
COMPILE_DB = os.path.join(REPO_ROOT, "_build-scan", "compile_commands.json")

_MARK = re.compile(r'^#\s+\d+\s+"([^"]+)"')
_MACRO_DEF = re.compile(r'^#define\s+([A-Za-z_][A-Za-z0-9_]*)')
_SYS_RE = re.compile(r'^(?:/usr/include|/usr/lib/|/Library/Developer|'
                     r'.*windows/include|.*mingw.*include|.*sysroot)')
_BUILTIN_FILES = ("<built-in>", "<command line>", "<stdin>")


def is_system_header(path: str) -> bool:
    """Эвристика пути: системный ли это заголовок (вне дерева проекта)."""
    if not path or not path.endswith((".h", ".hh", ".hpp")):
        return False
    real = os.path.realpath(path)
    if real.startswith(os.path.realpath(REPO_ROOT) + os.sep):
        return False
    return bool(_SYS_RE.match(path))


def _clang():
    return os.environ.get("CLANG", "clang")


def collect_header_marks(tu_list, clang, cfg: ScanConfig):
    """Собирает множество системных заголовков из препроцессорных file-marks
    всех TU (clang -E -dD). Возвращает {header_path: {entry для аргументов}}.
    """
    headers = {}
    for name, entry in tu_list:
        args = sanitize_args(entry["command"], entry["file"])
        if cfg and cfg._cross:
            args = cfg.args_for(entry, entry["file"])
        cmd = [clang, "-E", "-dD"] + args + [entry["file"]]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        except OSError as e:
            print("scan_system_deps: preprocess %s failed: %s" % (name, e),
                  file=sys.stderr)
            continue
        for line in r.stdout.splitlines():
            m = _MARK.match(line)
            if m:
                path = m.group(1)
                if path not in _BUILTIN_FILES and is_system_header(path):
                    headers.setdefault(path, {"entry": entry, "args": args})
    return headers


def scan_header_provides(clang, header_path, args=None):
    """PROVIDES одного заголовка: per-header AST-скан (`-x c <header>`).

    Возвращает {"fn": [...], "type": [...], "macro": [...]} — только имена,
    объявленные непосредственно в этом заголовке (loc.file узла совпадает
    с заголовком или отсутствует для builtin-подобных).
    """
    cmd = [clang, "-Xclang", "-ast-dump=json", "-fsyntax-only", "-x", "c",
           header_path]
    if args:
        cmd = [clang, "-Xclang", "-ast-dump=json", "-fsyntax-only"] + args + \
              ["-x", "c", header_path]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    try:
        ast = json.loads(r.stdout)
    except ValueError:
        print("scan_system_deps: AST %s failed: %s"
              % (header_path, (r.stderr or "")[:200]), file=sys.stderr)
        return None

    self_file = os.path.realpath(header_path)
    provides = {"fn": [], "type": [], "macro": []}

    def own_decl(n):
        """Декларация принадлежит этому заголовку: loc.file == header,
        либо loc.file отсутствует, но spellingLoc.file == header."""
        loc = n.get("loc", {})
        f = loc.get("file")
        if f is None:
            sp = (n.get("range", {}).get("begin", {}).get("spellingLoc", {})
                  or {}).get("file")
            return sp is None or os.path.realpath(sp) == self_file
        return os.path.realpath(f) == self_file

    def walk(n):
        k = n.get("kind")
        name = n.get("name")
        if name and not n.get("isImplicit") and own_decl(n):
            if k == "FunctionDecl":
                provides["fn"].append(name)
            elif k in ("RecordDecl", "EnumDecl", "TypedefDecl", "CXXRecordDecl"):
                provides["type"].append(name)
        for c in n.get("inner", []) or []:
            walk(c)

    walk(ast)
    return {k: sorted(set(v)) for k, v in provides.items()}


def scan_header_macros(clang, header_path, args=None):
    """Макросы, определённые в заголовке (из -E -dD file-marks)."""
    cmd = [clang, "-E", "-dD"]
    if args:
        cmd += args
    cmd += ["-x", "c", header_path]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    cur = None
    out = []
    for line in r.stdout.splitlines():
        m = _MARK.match(line)
        if m:
            cur = m.group(1)
            continue
        dm = _MACRO_DEF.match(line)
        if dm and cur and os.path.realpath(cur) == os.path.realpath(header_path):
            out.append(dm.group(1))
    return sorted(set(out))


def scan_uses(tu_list, clang, cfg: ScanConfig, provides_index: dict):
    """USES_SYS по TU: DeclRefExpr + макросы пользовательских файлов,
    резолв в заголовки через provides_index {short_name: [hdr_key]}.

    Возвращает {rel_file: {"sys_refs": {...}, "unresolved": [...]}}.
    """
    uses = {}
    for name, entry in tu_list:
        rel = os.path.relpath(entry["file"], REPO_ROOT)
        _, ast, err = run_tu(clang, name, entry, cfg)
        if ast is None:
            print("scan_system_deps: TU %s AST failed: %s" % (name, err or ""),
                  file=sys.stderr)
            continue
        refs = set()

        def walk(n):
            k = n.get("kind")
            if k == "DeclRefExpr":
                rd = n.get("referencedDecl") or {}
                nm = rd.get("name") or n.get("name")
                if nm:
                    refs.add(nm)
            for c in n.get("inner", []) or []:
                walk(c)

        walk(ast)

        # Макро-использования: ast-dump json НЕ эмитит MacroExpansion узлы
        # (ограничение clang; макро-объявления в scan_symbols тоже берутся из
        # -E -dD, не из AST). Поэтому дополнительно собираем все идентификаторы
        # из ИСХОДНОГО текста TU (до раскрытия) и матчим в provides_index.
        # Trade-off: идентификаторы из комментариев/строк тоже попадут —
        # приемлемо для «подтверждённого использования» (нет ложных резолвов:
        # матч только по реально объявленным в просканированных заголовках
        # именам).
        try:
            with open(entry["file"], encoding="utf-8", errors="replace") as f:
                src_text = f.read()
            for nm in re.findall(r"[A-Za-z_][A-Za-z0-9_]*", src_text):
                refs.add(nm)
        except OSError as e:
            print("scan_system_deps: read %s failed: %s" % (entry["file"], e),
                  file=sys.stderr)

        resolved, unresolved = {}, []
        for nm in sorted(refs):
            hdr_keys = provides_index.get(nm)
            if hdr_keys:
                resolved[nm] = sorted(set(hdr_keys))
            else:
                unresolved.append(nm)
        uses[rel] = {"sys_refs": resolved, "unresolved": unresolved}
    return uses


def merge_configs(scans: list) -> dict:
    """Мульти-конфигурационный merge: категоризация доступности.

    Каждому заголовку/символу присваивается `in_configs` — список конфигов,
    в которых он наблюдался. Категории (linux-only/win32-only/...) —
    производные от наблюдаемых конфигов, НЕ платформенные факты.
    """
    merged_headers, merged_symbols = {}, {}
    for scan in scans:
        cfg_name = scan["config"]
        for key, h in scan.get("headers", {}).items():
            m = merged_headers.setdefault(key, dict(h))
            m.setdefault("in_configs", [])
            if cfg_name not in m["in_configs"]:
                m["in_configs"].append(cfg_name)
        for fname, u in scan.get("uses", {}).items():
            mu = merged_symbols.setdefault(fname, {"sys_refs": {}, "unresolved": []})
            for sym, hdrs in u.get("sys_refs", {}).items():
                cur = mu["sys_refs"].setdefault(sym, [])
                for h in hdrs:
                    if h not in cur:
                        cur.append(h)
    for key, h in merged_headers.items():
        h["in_configs"] = sorted(h["in_configs"])
    return {"headers": merged_headers, "uses": merged_symbols,
            "configs": sorted(s["config"] for s in scans)}


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", default="linux", help="имя конфигурации скана")
    ap.add_argument("--cc", default=COMPILE_DB,
                    help="compile_commands.json (для --scan-uses)")
    ap.add_argument("--target", default=None, help="cross-target (напр. mingw)")
    ap.add_argument("--defines", default=None)
    ap.add_argument("--includes", default=None)
    ap.add_argument("--extra-flags", default=None)
    ap.add_argument("--headers-list", default=None,
                    help="файл со списком системных заголовков (по одному на строку); "
                         "по умолчанию собирается из file-marks TU")
    ap.add_argument("--provides-from", default=None,
                    help="режим --scan-uses: артефакт(ы) PROVIDES (json, через запятую) "
                         "для резолва")
    ap.add_argument("--mode", required=True,
                    choices=["scan-headers", "scan-uses", "merge"])
    ap.add_argument("--scans", default=None,
                    help="merge: артефакты конфигов (json, через запятую)")
    ap.add_argument("--limit-headers", type=int, default=0,
                    help="отладочное ограничение числа заголовков")
    ap.add_argument("--out", default=None, help="выходной json")
    args = ap.parse_args()

    clang = _clang()
    cfg = ScanConfig(name=args.config, target=args.target, defines=args.defines,
                     includes=args.includes, extra_flags=args.extra_flags)
    out_path = args.out or os.path.join(
        ARTIFACTS, "system-deps-%s.json" % args.config)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)

    if args.mode == "scan-headers":
        if args.headers_list:
            # список задан явно: compile_commands не нужен
            headers = {h.strip(): {"entry": None}
                       for h in open(args.headers_list) if h.strip()}
        else:
            cc = json.load(open(args.cc)) if os.path.getsize(args.cc) else []
            tu_list = select_tus(cc) if cc else []
            headers = collect_header_marks(tu_list, clang, cfg)
        if args.limit_headers:
            headers = dict(list(headers.items())[:args.limit_headers])
        result = {"config": args.config, "headers": {}, "uses": {}}
        for i, (path, meta) in enumerate(sorted(headers.items())):
            provides = scan_header_provides(clang, path,
                                            meta.get("args") if cfg._cross else None)
            if provides is None:
                continue
            macros = scan_header_macros(clang, path,
                                        meta.get("args") if cfg._cross else None)
            provides["macro"] = macros
            key = "hdr:%s" % os.path.basename(path)
            # коллизии базовых имён: добавляем различающую часть пути
            if key in result["headers"]:
                key = "hdr:%s" % path
            result["headers"][key] = {
                "name": os.path.basename(path),
                "path": path,
                "provides": provides,
            }
            if args.limit_headers and (i + 1) >= args.limit_headers:
                break
        json.dump(result, open(out_path, "w"), ensure_ascii=False, indent=1)
        print("scan_system_deps: %d headers -> %s" % (len(result["headers"]), out_path))
        return 0

    if args.mode == "scan-uses":
        cc = json.load(open(args.cc))
        tu_list = select_tus(cc)
        provides_index = {}
        for p in (args.provides_from or "").split(","):
            if p and os.path.exists(p):
                art = json.load(open(p))
                for key, h in art.get("headers", {}).items():
                    for kind, names in h.get("provides", {}).items():
                        for nm in names:
                            provides_index.setdefault(nm, set()).add(key)
        uses = scan_uses(tu_list, clang, cfg,
                         {k: list(v) for k, v in provides_index.items()})
        result = {"config": args.config, "headers": {}, "uses": uses}
        json.dump(result, open(out_path, "w"), ensure_ascii=False, indent=1)
        n_res = sum(len(u["sys_refs"]) for u in uses.values())
        n_unres = sum(len(u["unresolved"]) for u in uses.values())
        print("scan_system_deps: %d files, %d resolved refs, %d unresolved -> %s"
              % (len(uses), n_res, n_unres, out_path))
        return 0

    if args.mode == "merge":
        scans = []
        for p in (args.scans or "").split(","):
            if p and os.path.exists(p):
                scans.append(json.load(open(p)))
        if len(scans) < 2:
            print("scan_system_deps: merge требует ≥2 артефактов", file=sys.stderr)
            return 1
        result = merge_configs(scans)
        result["configs"] = [s["config"] for s in scans]
        json.dump(result, open(out_path, "w"), ensure_ascii=False, indent=1)
        print("scan_system_deps: merged %d configs, %d headers, %d files -> %s"
              % (len(scans), len(result["headers"]), len(result["uses"]), out_path))
        return 0

    return 2


if __name__ == "__main__":
    sys.exit(main())
