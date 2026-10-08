#!/usr/bin/env python3
"""load_map.py — загрузка refactoring-map.json в структурный слой Store.

Читает артефакт сканера (tools/artifacts/refactoring-map.json) и наполняет:
  - symbols:      ключ fq-имени -> JSON символа (функция/тип/макрос);
  - call_edges:   caller -> JSON {kind, resolved, ambiguous, callee} (DUPSORT);
  - call_edges_rev: callee -> caller (обратные рёбра для impact-анализа);
  - groups:       group:{kind}:{name} -> member_key (фаcеты подсистем);
  - regions:      region:{module}:{n} -> JSON (#if-дерево, 1131 регионов);
  - uncovered:    uncovered:{module}:{n} -> JSON (острова смысла вне union-AST).

`--replace` очищает перегенерируемые таблицы перед загрузкой (самоизлечение
дрейфа координат); без него — аддитивное дополнение.
`--skip-regions` пропускает regions/uncovered (только символы+рёбра).

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
    ap.add_argument("--replace", action="store_true",
                    help="очистить структурные таблицы перед загрузкой")
    ap.add_argument("--skip-regions", action="store_true",
                    help="не грузить regions/uncovered")
    ap.add_argument("--dry-run", action="store_true",
                    help="сверка только: не писать в БД")
    args = ap.parse_args()

    with open(args.map) as f:
        artifact = json.load(f)

    symbols = artifact["symbols"]
    edges = artifact["edges"]
    counts = artifact["counts"]
    regions = artifact.get("regions") or []
    uncovered = artifact.get("uncovered") or []
    print("artifact: %d symbols, %d edges, %d blocks, %d regions, %d uncovered" % (
        len(symbols), len(edges), counts["blocks"], len(regions), len(uncovered)),
        file=sys.stderr)

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
    loaded = store.map_load_batch(symbols, edges, groups,
                                  replace=args.replace)
    if not args.skip_regions and (regions or uncovered):
        r = store.map_load_regions(regions, uncovered, replace=args.replace)
        loaded.update(r)
    dt = time.time() - t0

    n_sym = len(store.map_symbols())
    n_reg = len(store.map_regions(limit=10**9))
    n_unc = len(store.map_uncovered(limit=10**9))
    # проверка обратных рёбер: случайное разрешённое ребро
    n_rev = 0
    if edges:
        sample = next(e for e in edges if e.get("resolved"))
        n_rev = len(store.map_callers_of(sample["callee"]))
    print("loaded: symbols=%d edges=%d groups=%d regions=%d uncovered=%d in %.1fs" % (
        loaded["symbols"], loaded["edges"], loaded["groups"],
        loaded.get("regions", 0), loaded.get("uncovered", 0), dt))
    print("verify: symbols=%d regions=%d uncovered=%d reverse_sample=%d" % (
        n_sym, n_reg, n_unc, n_rev))
    store.close()


if __name__ == "__main__":
    main()