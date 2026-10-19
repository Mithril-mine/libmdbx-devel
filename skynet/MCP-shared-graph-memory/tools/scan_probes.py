#!/usr/bin/env python3
"""scan_probes.py — сбор реестра probes из исходников и probe-bus.md.

Извлекает из `src/**`:
  - MPROBE_COLLECT / MPROBE_WATCH / MPROBE_FAULT (тег, файл, строка);
  - DEV_ASSERT / DEV_ASSERT_T (тег, файл, строка);
  - USDT-маркеры `mdbx::<subsystem>::<phase>::<event>` (из комментариев/кода).
и из `docs/engineering/probe-bus.md` — каталог USDT-пробников (§11.4).

Выход: tools/artifacts/probes.json (вне git), формат:
  [{"tag", "kind": usdt|bookmark|injected|dev_assert,
    "file", "line", "mechanism", "test_theme", "symbol"}]
Загрузка в память: `--db <path>` через Store.probe_batch (или MCP probe_batch).
"""

import argparse
import json
import os
import re
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, MODULE_DIR)
REPO_ROOT = os.path.dirname(os.path.dirname(MODULE_DIR))
SRC_DIR = os.path.join(REPO_ROOT, "src")
PROBE_BUS = os.path.join(REPO_ROOT, "docs", "engineering", "probe-bus.md")

_MPROBE_RE = re.compile(r"\bMPROBE_(COLLECT|WATCH|FAULT)\(\s*([A-Za-z_][\w]*)")
_DEVASSERT_RE = re.compile(r"\bDEV_ASSERT(?:_T)?\(\s*([A-Za-z_][\w]*)\s*,")
_USDT_RE = re.compile(r"\bmdbx::[a-z0-9_]+(?:::[a-z0-9_]+)+", re.I)

# механизмы по подкаталогам/файлам src
_MECHANISM_BY_FILE = {
    "gc-": "gc", "rkl": "gc", "spill": "spill", "txn-": "txn",
    "cursor": "cursor", "api-": "api", "lck-": "lck", "dbi-": "dbi",
    "page": "page", "dpl": "dpl", "osal": "osal", "mdbx.c": "core",
    "version": "core", "logging": "core", "options": "core",
}


def mechanism_of(file: str) -> str:
    base = os.path.basename(file)
    for pref, mech in _MECHANISM_BY_FILE.items():
        if base.startswith(pref):
            return mech
    return "core"


def scan_src(root: str) -> list:
    out = []
    for dirpath, _dirs, files in os.walk(root):
        for fname in sorted(files):
            if not fname.endswith((".c", ".c++", ".h", ".h++")):
                continue
            fpath = os.path.join(dirpath, fname)
            rel = os.path.relpath(fpath, REPO_ROOT)
            try:
                with open(fpath, errors="replace") as fh:
                    lines = fh.readlines()
            except OSError:
                continue
            for i, ln in enumerate(lines, 1):
                m = _MPROBE_RE.search(ln)
                if m:
                    kind_map = {"COLLECT": "collect", "WATCH": "watch",
                                "FAULT": "injected"}
                    out.append({
                        "tag": m.group(2), "kind": kind_map[m.group(1)],
                        "file": rel, "line": i, "mechanism": mechanism_of(fname),
                    })
                    continue
                m = _DEVASSERT_RE.search(ln)
                if m:
                    out.append({
                        "tag": m.group(1), "kind": "dev_assert",
                        "file": rel, "line": i, "mechanism": mechanism_of(fname),
                    })
                    continue
                for usdt in _USDT_RE.findall(ln):
                    out.append({
                        "tag": usdt.replace("::", "_"), "kind": "usdt",
                        "file": rel, "line": i, "mechanism": mechanism_of(fname),
                        "symbol": usdt,
                    })
    # dedup по (tag, kind): одна запись на тег (как реестр probe-bus)
    by_tag = {}
    for p in out:
        key = (p["tag"], p["kind"])
        if key not in by_tag:
            by_tag[key] = p
    return list(by_tag.values())


def scan_probe_bus(path: str) -> list:
    """Импорт каталога USDT-пробников из probe-bus.md (§11.4 таблица)."""
    out = []
    if not os.path.exists(path):
        return out
    with open(path, errors="replace") as fh:
        text = fh.read()
    # ищем строки таблицы `mdbx:...__...` с описанием
    for line in text.splitlines():
        if "mdbx:" in line and "`" in line:
            m = re.search(r"`(mdbx:[a-z0-9_]+)`", line)
            if m:
                out.append({
                    "tag": m.group(1).replace(":", "_").replace("__", "__"),
                    "kind": "usdt",
                    "file": line.split("`")[2] if line.count("`") >= 3 else "",
                    "line": 0,
                    "mechanism": "usdt",
                    "test_theme": "",
                })
    # dedup
    seen, uniq = set(), []
    for p in out:
        if p["tag"] not in seen:
            seen.add(p["tag"])
            uniq.append(p)
    return uniq


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=SRC_DIR)
    ap.add_argument("--probe-bus", default=PROBE_BUS)
    ap.add_argument("--out", default=os.path.join(TOOLS_DIR, "artifacts",
                                                  "probes.json"))
    ap.add_argument("--db", default=None,
                    help="если задан — сразу загрузить в память (probe_batch)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    t0 = time.time()
    probes = scan_src(args.src)
    usdt = scan_probe_bus(args.probe_bus)
    merged = probes + [p for p in usdt if p["tag"] not in {x["tag"] for x in probes}]
    counts = {"mpobe": len(probes), "usdt_from_doc": len(usdt), "total": len(merged)}
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as fh:
        json.dump(merged, fh, ensure_ascii=False, indent=1)
    print(json.dumps(counts))
    print("written:", args.out, "(%.1fs)" % (time.time() - t0))
    if args.dry_run:
        return 0
    if args.db:
        from mcp import Store  # noqa: PLC0415
        store = Store(args.db)
        try:
            r = store.probe_batch(merged)
            print("probe_batch:", r)
        finally:
            store.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())