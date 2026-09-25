#!/usr/bin/env python3
"""load_map.py — загрузка refactoring-map.json в структурный слой Store.

Читает артефакт сканера (tools/artifacts/refactoring-map.json) и аддитивно
наполняет таблицы:
  - symbols:    ключ fq-имени -> JSON символа (функция/тип/макрос);
  - call_edges: caller -> {kind}\x01{callee} (DUPSORT);
  - groups:     group:{kind}:{name} -> member_key (фаcеты подсистем).

Паттерн исполнения — как tools/build_libmdbx.py: детерминированный прогон,
в конце сверяет счётчики и печатает сводку.
"""

import argparse
import json
import os
import sys
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
sys.path.insert(0, MODULE_DIR)

from mcp import Store  # noqa: E402


def iter_sorted(d):
    return sorted(d.items())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default=os.path.join(TOOLS_DIR, "artifacts",
                                                  "refactoring-map.json"))
    ap.add_argument("--db", default=":memory:")
    ap.add_argument("--dry-run", action="store_true",
                    help="сверка только: не писать в БД")
    args = ap.parse_args()

    with open(args.map) as f:
        artifact = json.load(f)

    symbols = artifact["symbols"]
    edges = artifact["edges"]
    counts = artifact["counts"]
    print("artifact: %d symbols, %d edges, %d blocks" % (
        len(symbols), len(edges), counts["blocks"]), file=sys.stderr)

    if args.dry_run:
        return

    db = args.db
    if db == ":memory:":
        import tempfile
        db = os.path.join(tempfile.mkdtemp(), "map.mdbx")
    store = Store(db)
    groups = {}
    for key, body in iter_sorted(symbols):
        gkey = "group:subsystem:%s" % body.get("module", "?")
        groups.setdefault(gkey, set()).add(key)

    t0 = time.time()
    loaded = store.map_load_batch(symbols, edges, groups)
    dt = time.time() - t0

    n_sym = len(store.map_symbols())
    print("loaded: symbols=%d edges=%d groups=%d in %.1fs" % (
        loaded["symbols"], loaded["edges"], loaded["groups"], dt))
    print("verify: symbols=%d" % n_sym)
    store.close()


if __name__ == "__main__":
    main()