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

Существенное ограничение (документировано, см. docs/CODEGRAPH.md):
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


def _drop_multi_arch(args: list) -> list:
    """macOS universal-binary compile_commands несут несколько `-arch` —
    `-E`/`-H` с ними несовместимы ('cannot use cpp-output with multiple
    -arch options'). Оставляем только первый."""
    out, seen_arch = [], False
    i, n = 0, len(args)
    while i < n:
        if args[i] == "-arch":
            if not seen_arch:
                out += args[i:i + 2]
                seen_arch = True
            i += 2
            continue
        if args[i].startswith("-arch"):
            if not seen_arch:
                out.append(args[i])
                seen_arch = True
            i += 1
            continue
        out.append(args[i])
        i += 1
    return out


def scan_header_provides(clang, header_path, args=None):
    """PROVIDES одного заголовка: per-header AST-скан (`-x c <header>`).

    Возвращает {"fn": [...], "type": [...], "macro": [...]} — только имена,
    объявленные непосредственно в этом заголовке (loc.file узла совпадает
    с заголовком или отсутствует для builtin-подобных). None — не удалось
    получить AST (детали в stderr).
    """
    base = [clang, "-Xclang", "-ast-dump=json", "-fsyntax-only"]
    attempts = []
    if args:
        attempts.append(_drop_multi_arch(list(args)))
    attempts.append([])  # fallback: голый clang (Windows auto-detect MSVC)
    last_err = ""
    for extra in attempts:
        cmd = base + extra + ["-x", "c", header_path]
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        try:
            ast = json.loads(r.stdout)
        except ValueError:
            last_err = (r.stderr or "").strip().splitlines()[-1] if (r.stderr or "").strip() else "empty AST"
            continue
        provides = {"fn": [], "type": [], "macro": []}
        self_file = os.path.realpath(header_path)

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
    print("scan_system_deps: AST %s failed: %s"
          % (os.path.basename(header_path), last_err[:160]), file=sys.stderr)
    return None


def scan_header_macros(clang, header_path, args=None):
    """Макросы, определённые в заголовке (из -E -dD file-marks)."""
    cmd = [clang, "-E", "-dD"]
    if args:
        cmd += _drop_multi_arch(args)
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
                    choices=["scan-headers", "scan-uses", "merge", "collect-includes"])
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
        # Источник списка:
        #  - --headers-list (формат: путь [имя]) — для CI/повторяемости;
        #  - иначе collect_header_marks по compile_commands.
        # Записи, которых нет на этой платформе, фиксируются в artifact.absent
        # (честно: платформенно-отсутствующий заголовок — тоже факт модели).
        absent = []
        headers = {}
        if args.headers_list is None:
            # Без --headers-list: собираем системные заголовки из
            # препроцессорных file-marks всех TU (clang -E -dD).
            cc_data = json.load(open(args.cc)) if os.path.getsize(args.cc) else []
            tu_list = select_tus(cc_data) if cc_data else []
            if not tu_list:
                print("scan_system_deps: scan-headers: нет TU (compile_commands?)",
                      file=sys.stderr)
                return 1
            headers.update(collect_header_marks(tu_list, clang, cfg))
            if args.limit_headers:
                headers = dict(list(headers.items())[:args.limit_headers])
            absent = []
        if args.headers_list is not None:
            entries = []  # (requested, display_name)
            for line in open(args.headers_list):
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split()
                hpath = parts[0]
                hname = parts[1] if len(parts) > 1 else os.path.basename(hpath)
                entries.append((hpath, hname))
            # Резолв имён в абсолютные пути средствами clang целевой платформы.
            # КАЖДЫЙ заголовок резолвится ОТДЕЛЬНЫМ мини-TU (`#include <name>`
            # + `-H`): отказ одного (платформенно-отсутствующий) не влияет на
            # остальные — общий TU оборвался бы fatal error'ом на первом же
            # отсутствующем. Дополнительные флаги берутся из compile_commands
            # (если существует; MSVC-генератор его не создаёт — clang на
            # Windows находит MSVC-заголовки через auto-detect). Не найденные
            # платформой честно фиксируются в absent (факт модели).
            resolved_by_name = {}
            resolve_debug = []
            if entries:
                args0 = []
                if args.cc and os.path.isfile(args.cc) and os.path.getsize(args.cc):
                    cc = json.load(open(args.cc))
                    tu_list = select_tus(cc) if cc else []
                    if tu_list:
                        args0 = sanitize_args(tu_list[0][1]["command"],
                                              tu_list[0][1]["file"])
                        if cfg._cross:
                            args0 = cfg.args_for(tu_list[0][1],
                                                 tu_list[0][1]["file"])
                        args0 = _drop_multi_arch(args0)
                tmpdir_hdr = os.path.dirname(out_path) or "."
                tree_re = re.compile(r'^[. ]*\s(.+?)\s*$')
                for hpath, hname in entries:
                    probe = os.path.join(tmpdir_hdr,
                                         "_sysdeps-probe-%s.c" % args.config)
                    with open(probe, "w") as f:
                        f.write("#include <%s>\n" % hpath)
                    try:
                        r = subprocess.run([clang, "-E", "-H"] + args0 + [probe],
                                           capture_output=True, text=True,
                                           errors="replace")
                    except FileNotFoundError:
                        resolve_debug.append("clang binary not found: %r" % clang)
                        break
                    os.unlink(probe)
                    if len(resolve_debug) < 3:
                        # диагностика первых резолв-прогонов (CI-лог): rc и
                        # первые строки stderr — без этого «0 headers»
                        # недиагностируем
                        head = (r.stderr or "").splitlines()[:4]
                        resolve_debug.append("probe <%s>: rc=%d stderr[:4]=%s"
                                             % (hpath, r.returncode, head))
                    for line in (r.stderr or "").splitlines():
                        m = tree_re.match(line)
                        if not m:
                            continue
                        p = m.group(1)
                        if os.path.basename(p) == hname or p.endswith("/" + hpath) \
                           or p.endswith("\\" + hpath) or p == hpath:
                            if hname not in resolved_by_name:
                                resolved_by_name[hname] = p
                            break
                for d in resolve_debug:
                    print("scan_system_deps: resolve-debug: %s" % d,
                          file=sys.stderr)
            for hpath, hname in entries:
                if os.path.isabs(hpath) and os.path.exists(hpath):
                    headers[hpath] = {"name": hname, "entry": None}
                    continue
                resolved = resolved_by_name.get(hname) or resolved_by_name.get(hpath)
                if resolved:
                    headers[resolved] = {"name": hname, "entry": None}
                else:
                    # платформенно-отсутствующий: важный факт для категории
                    absent.append({"name": hname, "path": hpath,
                                   "reason": "header not found on this platform (clang include paths)"})
        if args.limit_headers:
            headers = dict(list(headers.items())[:args.limit_headers])
        print("scan_system_deps: resolved=%d absent-notfound=%d"
              % (len(headers), len(absent)), file=sys.stderr)
        result = {"config": args.config, "headers": {}, "uses": {},
                  "absent": absent}
        import sys as _sys
        print("scan_system_deps: DEBUG headers-before-loop=%d" % len(headers),
              file=_sys.stderr)
        for i, (path, meta) in enumerate(sorted(headers.items())):
            provides = scan_header_provides(clang, path,
                                            meta.get("args") if cfg._cross else None)
            if provides is None:
                # заголовок найден, но не компилируется в этом конфиге
                # (напр. C++-заголовок под -x c): честный факт, не молчание
                absent.append({"name": os.path.basename(path), "path": path,
                               "reason": "header exists but failed to compile "
                                         "in this config (see scan stderr)"})
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
        # Сохраняем headers из provides_from (это резолв-артефакт scan-headers),
        # чтобы merge и sys_deps_load видели PROVIDES-таблицу.
        carried_headers = {}
        for p in (args.provides_from or "").split(","):
            if p and os.path.exists(p):
                for key, h in json.load(open(p)).get("headers", {}).items():
                    carried_headers[key] = h
        result = {"config": args.config, "headers": carried_headers, "uses": uses}
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

    if args.mode == "collect-includes":
        """Угловые include из исходников проекта + резолв путей через clang.

        Собирает все `#include <...>` из src/** и tests/**, резолвит каждый
        через clang (`-E -H -fsyntax-only` на минимальном TU: путь из
        preprocessed file-marks), выводит список «путь [имя]» для
        --headers-list режима scan-headers. Заголовки, отсутствующие на этой
        платформе, в список не попадают (их отсутствие фиксирует
        scan-headers/absent на платформе, где они есть, либо merge).
        """
        cc = json.load(open(args.cc)) if os.path.getsize(args.cc) else []
        tu_list = select_tus(cc) if cc else []
        if not tu_list:
            print("scan_system_deps: collect-includes: нет TU (compile_commands?)",
                  file=sys.stderr)
            return 1
        name, entry = tu_list[0]
        args0 = sanitize_args(entry["command"], entry["file"])
        if cfg._cross:
            args0 = cfg.args_for(entry, entry["file"])
        # 1) угловые include из исходников проекта
        angle = set()
        inc_re = re.compile(r'^\s*#\s*include\s*<([^>]+)>')
        roots = [os.path.join(REPO_ROOT, "src"),
                 os.path.join(REPO_ROOT, "tests")]
        for root in roots:
            for dirpath, _, files in os.walk(root):
                for fn in files:
                    if not fn.endswith((".c", ".h", ".c++", ".h++", ".cxx")):
                        continue
                    try:
                        with open(os.path.join(dirpath, fn), encoding="utf-8",
                                  errors="replace") as f:
                            for line in f:
                                m = inc_re.match(line)
                                if m:
                                    angle.add(m.group(1))
                    except OSError:
                        continue
        print("scan_system_deps: %d angle-includes from src/tests" % len(angle),
              file=sys.stderr)
        # 2) резолв: clang -E -H по минимальному TU, включающему их все
        probe = os.path.join(os.path.dirname(out_path) or ".",
                             "_sysdeps-probe-%s.c" % args.config)
        with open(probe, "w") as f:
            for inc in sorted(angle):
                f.write("#include <%s>\n" % inc)
        cmd = [clang, "-E", "-H"] + args0 + [probe]
        r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
        os.unlink(probe)
        resolved, failed = [], []
        for line in (r.stderr or "").splitlines():
            # clang -H печатает дерево: ". ./path/header.h" / ".. ./path2..."
            m = re.match(r'^[. ]*\s(\S+)$', line)
            if not m:
                continue
            p = m.group(1)
            base = os.path.basename(p)
            if base in angle or p.endswith(tuple(angle)):
                if os.path.exists(p):
                    resolved.append(p)
        for inc in sorted(angle):
            if not any(inc == os.path.basename(p) or p.endswith("/" + inc)
                       for p in resolved):
                failed.append(inc)
        lines = ["%s %s" % (p, os.path.basename(p)) for p in sorted(resolved)]
        with open(out_path, "w") as f:
            f.write("\n".join(lines) + ("\n" if lines else ""))
        print("scan_system_deps: collect-includes: %d resolved, %d not-found "
              "(%s) -> %s" % (len(resolved), len(failed),
                              ", ".join(sorted(failed)[:6]) or "-", out_path))
        # не-найденные на этой платформе печатаем явно (факт для другой платформы)
        for inc in sorted(failed):
            print("scan_system_deps:   absent here: <%s>" % inc, file=sys.stderr)
        return 0

    return 2


if __name__ == "__main__":
    sys.exit(main())
