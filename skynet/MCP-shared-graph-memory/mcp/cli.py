"""memory-cli — утилиты обслуживания хранилища без MCP.

Команды: dump, stats, graph, audit, gc, vocab, purge, test-metrics, coverage,
ci-jobs, backup (pin-точка), export (наружу), upgrade (схема-миграция).
"""

from __future__ import annotations

import argparse
import json
import sys

from .mcp_server import _default_db_path
from .store import Store


def _open(path: str) -> Store:
    return Store(path)


def cmd_dump(args) -> None:
    s = _open(args.path)
    try:
        if args.key:
            rec = s.get_record(args.key)
            print(json.dumps(rec, ensure_ascii=False, indent=2) if rec else "NOTFOUND")
            return
        keys = s.keys("")
        if args.type:
            keys = [k for k in keys if k.startswith(args.type + ":")]
        if args.module:
            keys = [k for k in keys if (":" + args.module + ":") in k]
        for k in keys:
            rec = s.get_record(k)
            print("%s\t%s\t%s\t%s" % (k, rec.get("type"), rec.get("importance"),
                                      rec.get("summary", "")[:80]))
    finally:
        s.close()


def cmd_stats(args) -> None:
    s = _open(args.path)
    try:
        print(json.dumps(s.stats(), ensure_ascii=False, indent=2))
    finally:
        s.close()


def cmd_version(args) -> None:
    with _open(args.path) as s:
        print(json.dumps(s.version_info(), ensure_ascii=False, indent=2))


def cmd_graph(args) -> None:
    s = _open(args.path)
    try:
        g = s.graph(args.key, args.depth)
        if args.format == "dot":
            print("digraph memory {")
            for e in g["edges"]:
                print('  "%s" -> "%s" [label="%s"];'
                      % (e["subject"], e["object"], e["predicate"]))
            print("}")
        else:
            print(json.dumps(g, ensure_ascii=False, indent=2))
    finally:
        s.close()


def cmd_audit(args) -> None:
    s = _open(args.path)
    try:
        print(json.dumps({"note": "audit: см. gc --dry-run и stats",
                          "records": len(s.keys(""))},
                         ensure_ascii=False, indent=2))
    finally:
        s.close()


def cmd_gc(args) -> None:
    s = _open(args.path)
    try:
        res = s.gc(dry_run=not args.execute, archive=args.archive,
                   threshold=args.threshold)
        print(json.dumps(res, ensure_ascii=False, indent=2))
    finally:
        s.close()


def cmd_vocab(args) -> None:
    s = _open(args.path)
    try:
        if args.add == "module":
            print(s.vocab_add(args.value))
        elif args.add == "topic":
            module, _, topic = args.value.partition(":")
            print(s.vocab_add(module.strip(), topic.strip()))
        elif args.find:
            print(json.dumps(s.vocab_find(args.find), ensure_ascii=False, indent=2))
        else:
            print(json.dumps(s.vocab_list(), ensure_ascii=False, indent=2))
    finally:
        s.close()


def cmd_purge(args) -> None:
    s = _open(args.path)
    try:
        print(json.dumps(s.purge(args.keys), ensure_ascii=False, indent=2))
    finally:
        s.close()


def cmd_test_metrics(args) -> None:
    s = _open(args.path)
    try:
        if args.test:
            res = s.test_metrics_get(args.test)
            print(json.dumps(res, ensure_ascii=False, indent=2) if res else "NOTFOUND")
        else:
            print(json.dumps(s.test_metrics_all(args.prefix, args.limit),
                             ensure_ascii=False, indent=2))
    finally:
        s.close()


def cmd_coverage(args) -> None:
    s = _open(args.path)
    try:
        if args.test:
            res = s.coverage_get(args.test)
            print(json.dumps(res, ensure_ascii=False, indent=2) if res else "NOTFOUND")
        else:
            print(json.dumps(s.coverage_all(args.limit), ensure_ascii=False, indent=2))
    finally:
        s.close()


def cmd_ci_jobs(args) -> None:
    s = _open(args.path)
    try:
        print(json.dumps(s.ci_jobs_all(args.prefix, args.limit),
                         ensure_ascii=False, indent=2))
    finally:
        s.close()


def cmd_backup(args) -> None:
    """Внутренняя точка сохранения (pin-*): db_backup."""
    with _open(args.path) as s:
        r = s.backup(kind="pin", force=args.force)
        print(json.dumps(r, ensure_ascii=False, indent=2))


def cmd_export(args) -> None:
    """Экспорт копии НАРУЖУ каталога (внешняя архивация/перенос)."""
    with _open(args.path) as s:
        r = s.export(args.dest)
        print(json.dumps(r, ensure_ascii=False, indent=2))


def cmd_upgrade(args) -> None:
    """Схема-миграция (новые таблицы, INCOMPATIBLE-пересоздание, backfill)."""
    with Store(args.path, readonly=False) as s:
        r = s.upgrade()
        print(json.dumps(r, ensure_ascii=False, indent=2))


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    ap = argparse.ArgumentParser(prog="memory-cli")
    ap.add_argument("--path", default=None)
    sub = ap.add_subparsers(dest="cmd")

    p = sub.add_parser("dump")
    p.add_argument("--type", default="")
    p.add_argument("--module", default="")
    p.add_argument("--key", default="")
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser("stats")
    p.set_defaults(func=cmd_stats)

    p = sub.add_parser("version")
    p.set_defaults(func=cmd_version)

    p = sub.add_parser("graph")
    p.add_argument("--key", required=True)
    p.add_argument("--depth", type=int, default=1)
    p.add_argument("--format", choices=["json", "dot"], default="json")
    p.set_defaults(func=cmd_graph)

    p = sub.add_parser("audit")
    p.set_defaults(func=cmd_audit)

    p = sub.add_parser("gc")
    p.add_argument("--execute", action="store_true")
    p.add_argument("--archive", action="store_true")
    p.add_argument("--threshold", type=float, default=0.3)
    p.set_defaults(func=cmd_gc)

    p = sub.add_parser("vocab")
    p.add_argument("--add", choices=["module", "topic"])
    p.add_argument("--value", default="")
    p.add_argument("--find", default="")
    p.set_defaults(func=cmd_vocab)

    p = sub.add_parser("purge")
    p.add_argument("keys", nargs="+")
    p.set_defaults(func=cmd_purge)

    p = sub.add_parser("test-metrics")
    p.add_argument("--test", default="")
    p.add_argument("--prefix", default="")
    p.add_argument("--limit", type=int, default=None)
    p.set_defaults(func=cmd_test_metrics)

    p = sub.add_parser("coverage")
    p.add_argument("--test", default="")
    p.add_argument("--limit", type=int, default=None)
    p.set_defaults(func=cmd_coverage)

    p = sub.add_parser("ci-jobs")
    p.add_argument("--prefix", default="")
    p.add_argument("--limit", type=int, default=None)
    p.set_defaults(func=cmd_ci_jobs)

    p = sub.add_parser("backup")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_backup)

    p = sub.add_parser("export")
    p.add_argument("dest")
    p.set_defaults(func=cmd_export)

    p = sub.add_parser("upgrade")
    p.set_defaults(func=cmd_upgrade)

    args = ap.parse_args(argv)
    if not getattr(args, "cmd", None):
        ap.print_help()
        return 1
    if not args.path:
        args.path = _default_db_path()
    args.func(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
