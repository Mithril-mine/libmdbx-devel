#!/usr/bin/env python3
"""collect_traces.py — загрузка наблюдаемых вызовов из дампа в call_traces.

Форматы входа (авто-определение):
  1. JSON: [{"caller": "fn:...", "callee": "fn:...", "count": N,
             "label": "..."}]  (или {"pairs": [...]});
  2. Текстовый дамп probe-bus/счётчиков — строки вида:
       caller callee [count] [label]
     (пробел/tab-разделители; строки с # и пустые пропускаются).

Назначение: после инструментированного прогона (probe-bus, USDT, счётчики)
зафиксировать observed-вызовы, чтобы `trace_compare` показывал possible vs
observed для символа — сигнал уверенности при портировании C→C++.

Использование:
  python3 tools/collect_traces.py --dump traces.jsonl --db <path> [--layer L]
"""

import argparse
import json
import os
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODULE_DIR = os.path.dirname(TOOLS_DIR)
if MODULE_DIR not in sys.path:
    sys.path.insert(0, MODULE_DIR)

from mcp import Store  # noqa: E402


def parse_dump(path: str) -> list:
    """JSON-массив/объект или построчный текстовый дамп → список пар."""
    with open(path, errors="replace") as fh:
        text = fh.read().strip()
    if not text:
        return []
    # JSON?
    if text[0] in "[{":
        try:
            data = json.loads(text)
        except ValueError:
            data = None
        if isinstance(data, list):
            return [p for p in data if isinstance(p, dict)]
        if isinstance(data, dict) and isinstance(data.get("pairs"), list):
            return [p for p in data["pairs"] if isinstance(p, dict)]
        if data is not None:
            return []
    # JSONL (по строке — объект)?
    pairs = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("{"):
            try:
                obj = json.loads(line)
                if isinstance(obj, dict):
                    pairs.append(obj)
                continue
            except ValueError:
                pass
        parts = line.replace("\t", " ").split()
        if len(parts) < 2:
            continue
        rec = {"caller": parts[0], "callee": parts[1]}
        if len(parts) >= 3:
            try:
                rec["count"] = int(parts[2])
            except ValueError:
                rec["label"] = parts[2]
        if len(parts) >= 4:
            rec["label"] = parts[3]
        pairs.append(rec)
    return pairs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump", required=True,
                    help="файл дампа (json/jsonl/text)")
    ap.add_argument("--db", default=os.path.join(
        os.path.expanduser("~"), ".local", "share", "shared-graph-memory",
        "db.mdbx"))
    ap.add_argument("--layer", default="",
                    help="слой ветки (пустой — активная/базовый)")
    ap.add_argument("--label", default="",
                    help="метка источника для всех пар без своего label")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    pairs = parse_dump(args.dump)
    if args.label:
        for p in pairs:
            p.setdefault("label", args.label)
    print("parsed %d pairs from %s" % (len(pairs), args.dump))
    if args.dry_run:
        for p in pairs[:20]:
            print("  %s -> %s (x%s)" % (p.get("caller"), p.get("callee"),
                                        p.get("count", 1)))
        return 0
    if not pairs:
        return 0

    with Store(args.db) as store:
        r = store.trace_ingest_batch(pairs, layer=args.layer)
    print("trace_ingest_batch:", r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
