#!/usr/bin/env python3
"""Экспорт проектных знаний живой MCP-памяти в git (skynet/, самодостаточный слой).

Читает живую БД shared-graph-memory (read-only) и генерирует:
  skynet/knowledge-records.yaml  — машинный дамп (records + links + vocab);
  skynet/knowledge-map.md        — человеко/агент-читаемая карта знаний (мета).

Классификация: в git уходят проектные и мета-записи (core/build/testing/tools/
memory/practice/meta/bug/decision/proc по разрешённым префиксам), а роевые
(fact:swarm:*), события (event:*), сессионное состояние (context-breadcrumbs,
state) и рабочие todo — остаются вне git (в живой памяти / workspace).

Регенерация: python3 tools/export-knowledge.py [--db PATH] [--out DIR]
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mcp import Store  # noqa: E402
from mcp import libmdbx as mdbx  # noqa: E402

MODULE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_DB = os.path.expanduser("~/.local/share/shared-graph-memory/db.mdbx")
DEFAULT_OUT = os.path.join(MODULE_DIR, "..")  # репозиторий/skynet

ALLOWED_PREFIXES = (
    "fact:core:", "fact:build:", "fact:testing:", "fact:tools:",
    "fact:meta:", "fact:memory:", "fact:practice:",
    "decision:memory:", "decision:meta:",
    "proc:memory:", "proc:meta:", "proc:practice:",
    "bug:core:",
)
EXCLUDED_KEYS = {
    # сессионное/рабочее состояние — вне git
    "proc:meta:context-breadcrumbs",
    "proc:meta:state",
    "proc:todo:mcpx-diagnostics",
    # агентская методология (рой), доки в workspace
    "proc:practice:superpowers-integration",
}


def included(key: str) -> bool:
    if key in EXCLUDED_KEYS:
        return False
    return any(key.startswith(p) for p in ALLOWED_PREFIXES)


def collect(store: Store) -> tuple:
    records = {}
    with store.env.begin(readonly=True) as txn:
        cur = txn.cursor(store.dbi(txn, "records"))
        rc, k, v = cur.get(mdbx.CURSOR_FIRST)
        while rc == mdbx.RC_SUCCESS:
            key = k.decode()
            if included(key):
                body = json.loads(v.decode())
                records[key] = {
                    "key": key,
                    "type": key.split(":", 1)[0],
                    "date": body.get("date"),
                    "importance": body.get("importance"),
                    "summary": body.get("summary"),
                }
            rc, k, v = cur.get(mdbx.CURSOR_NEXT)
        vocab = {"modules": [], "topics": []}
        cur = txn.cursor(store.dbi(txn, "vocab"))
        rc, k, _ = cur.get(mdbx.CURSOR_FIRST)
        while rc == mdbx.RC_SUCCESS:
            text = k.decode()
            if text.startswith("module:"):
                vocab["modules"].append(text[7:])
            elif ":" in text:
                vocab["topics"].append(text)
            rc, k, _ = cur.get(mdbx.CURSOR_NEXT)
    # связи только между включёнными записями
    links = []
    for key in records:
        try:
            g = store.graph(key, depth=1)
        except Exception:  # noqa: BLE001
            continue
        for e in g["edges"]:
            if e["subject"] in records and e["object"] in records:
                links.append(e)
    seen = set()
    uniq = []
    for e in links:
        sig = (e["subject"], e["predicate"], e["object"])
        if sig not in seen:
            seen.add(sig)
            uniq.append(e)
    return sorted(records.values(), key=lambda r: r["key"]), uniq, vocab


def write_yaml(path: str, records, links, vocab) -> None:
    lines = [
        "# Экспорт проектных знаний живой MCP-памяти (генерируется).",
        "# Регенерация: python3 skynet/MCP-shared-graph-memory/tools/export-knowledge.py",
        f"# generated_at: {time.strftime('%Y-%m-%d %H:%M UTC')}",
        "",
        "vocab:",
        "  modules:",
    ]
    for m in sorted(vocab["modules"]):
        lines.append(f"    - {m}")
    lines.append("  topics:")
    for t in sorted(vocab["topics"]):
        lines.append(f"    - {t}")
    lines.append("")
    lines.append("records:")
    for r in records:
        lines.append(f"- key: {r['key']}")
        lines.append(f"  type: {r['type']}")
        if r["date"]:
            lines.append(f"  date: {r['date']}")
        if r["importance"] is not None:
            lines.append(f"  importance: {r['importance']}")
        summary = (r["summary"] or "").strip().replace("\n", " ")
        lines.append(f"  summary: \"{summary}\"")
    lines.append("")
    lines.append("links:")
    for e in links:
        lines.append(f"- subject: {e['subject']}")
        lines.append(f"  predicate: {e['predicate']}")
        lines.append(f"  object: {e['object']}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def write_map(path: str, records, links, vocab) -> None:
    by_domain = {}
    for r in records:
        domain = r["key"].split(":", 2)[1]
        by_domain.setdefault(domain, []).append(r)
    by_link = {}
    for e in links:
        by_link.setdefault(e["subject"], []).append(
            "%s -> %s" % (e["predicate"], e["object"]))
    L = ["# Knowledge map (meta) — libmdbx shared knowledge",
         "",
         "Слой знаний о знаниях для работы над проектом. Самодостаточен:",
         "агент с чистым клоном этого репозитория находит здесь всё нужное.",
         "",
         "_Сгенерирован_: `skynet/MCP-shared-graph-memory/tools/export-knowledge.py`.",
         "_Регенерация после изменений живой памяти_: те же команда.",
         "",
         "## Где что лежит",
         "",
         "| Куда | Что |",
         "| --- | --- |",
         "| `docs/engineering/` | человеко-ориентированные доки (архитектура, сборка, структура, C++ API, тесты, бэклог техдолга…) |",
         "| `skynet/knowledge-records.yaml` | машинный экспорт записей + связей + словаря |",
         "| этот файл | карта знаний (мета) |",
         "| `skynet/MCP-shared-graph-memory/` | модуль памяти (Store, схема, SKILL, тулзы) |",
         "| `skynet/git-history-policy.md` | правило перезаписи истории/публикации |",
         "| вне git: `/sourcecraft/workspace/skynet/` | роевое (протоколы, роли, миссия, координация) + BOOTSTRAP (операционная точка входа) |",
         "",
         "## Словарь доменов",
         "",
         "| Домен | Содержание |",
         "| --- | --- |",
         "| core | инварианты движка, структуры данных, синхронизация, слоты читателей, меты, traps, perf, USDT |",
         "| build | опции сборки, CMake-особенности |",
         "| testing | организация тестов, CI-состояние, профиль длительностей |",
         "| tools | фиксы инструментов (mdbx_load stdin и т.п.) |",
         "| practice | проверенные clang-инварианты сканера, консультации Алисы |",
         "| memory | дизайн/решения модуля MCP-памяти (backup-tiers, readonly, durability, инцидент) |",
         "| meta | знания о знаниях: карта, паспорт данных карты, инвентарь архива, жизненный цикл AST |",
         "| bug | известные баги |",
         "",
         "## Записи по доменам",
         ""]
    for domain in sorted(by_domain):
        L.append("### %s" % domain)
        L.append("")
        for r in by_domain[domain]:
            imp = r["importance"] if r["importance"] is not None else "?"
            L.append("- **`%s`** (%s, %s, imp %s): %s"
                     % (r["key"], r["type"], r["date"] or "?",
                        imp, (r["summary"] or "").strip()))
            for rel in by_link.get(r["key"], []):
                L.append("    - `%s`" % rel)
        L.append("")
    L += [
        "## Провенанс",
        "",
        "- Архив роя `.archive/mcp-memory-dump.yaml` (25.09, 126 сущностей/104 связи) —",
        "  дистилляция в домены выполнена 27.09 (`proc:meta:archive-distillation`);",
        "  рабочее состояние роя (ящики/реестры/статусы) в знания не переносится.",
        "- Данные refactoring-map верифицированы (`fact:meta:map-data-passport`).",
        "- События после дистилляции (CI-фикс, консолидация веток) — в живой памяти;",
        "  журнал ведётся также в `/sourcecraft/workspace/BOOTSTRAP.md`.",
        "",
        "## Процедура обновления",
        "",
        "1. Внести изменения через `safe_store`/`recall` (SKILL.md модуля).",
        "2. Перегенерировать: `python3 skynet/MCP-shared-graph-memory/tools/export-knowledge.py`.",
        "3. Закоммитить обновлённые `skynet/knowledge-records.yaml` и этот файл.",
        "",
    ]
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(L))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--out", default=DEFAULT_OUT)
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    store = Store(args.db, readonly=True, _skip_sync_thread=True)
    try:
        records, links, vocab = collect(store)
    finally:
        store.close()
    os.makedirs(out, exist_ok=True)
    write_yaml(os.path.join(out, "knowledge-records.yaml"), records, links, vocab)
    write_map(os.path.join(out, "knowledge-map.md"), records, links, vocab)
    print("exported %d records, %d links, %d modules, %d topics -> %s"
          % (len(records), len(links), len(vocab["modules"]),
             len(vocab["topics"]), out))
    return 0


if __name__ == "__main__":
    sys.exit(main())