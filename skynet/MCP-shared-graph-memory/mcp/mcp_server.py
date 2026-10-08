"""MCP-сервер памяти роя (JSON-RPC 2.0 поверх stdio).

Транспорт ошибок: контрактные ``error$CODE | CLASS=... | DESC=... | ACTION=...
| RETRY=...`` возвращаются как JSON-RPC protocol error (-32000), т.к. это
единственный гарантированный способ донести ошибку до модели в opencode.
"""

from __future__ import annotations

import json
import os
import sys

from . import libmdbx as mdbx
from .errors import MemoryError
from .store import Store


def _default_db_path() -> str:
    env = os.environ.get("SHARED_GRAPH_MEMORY_PATH")
    if env:
        return env
    return os.path.join(os.path.expanduser("~"), ".local", "share",
                        "shared-graph-memory", "db.mdbx")


class McpServer:
    def __init__(self, store: Store):
        self.store = store
        self.tools = self._build_tools()

    # --- инструменты ---------------------------------------------------------
    def _build_tools(self) -> dict:
        t = [
            ("normalize_key", "Нормализация произвольной строки в канонический ключ "
             "тип:модуль:тема (проверка словаря).", ["raw"], {
                 "type": "object",
                 "properties": {"raw": {"type": "string"}},
                 "required": ["raw"],
             }),
            ("safe_store", "Атомарное сохранение записи с дедупликацией (1 write-txn). "
             "Ответ: created/merged/conflict.", ["key", "type", "summary", "importance"], {
                 "type": "object",
                 "properties": {
                     "key": {"type": "string"},
                     "type": {"type": "string", "enum": ["decision", "bug", "proc",
                                                        "bottleneck", "fact", "event"]},
                     "summary": {"type": "string"},
                     "importance": {"type": "number"},
                     "date": {"type": "string"},
                 },
                 "required": ["key", "type", "summary", "importance"],
             }),
            ("recall", "Поиск по префиксу ключа с ранжированием "
             "(pattern вида 'bug:crypto:*').", ["pattern"], {
                 "type": "object",
                 "properties": {"pattern": {"type": "string"}, "limit": {"type": "integer"}},
                 "required": ["pattern"],
             }),
            ("search", "Поиск по термам (пересечение постинг-списков + ранжирование).",
             ["query"], {
                 "type": "object",
                 "properties": {"query": {"type": "string"}, "limit": {"type": "integer"}},
                 "required": ["query"],
             }),
            ("lookup", "Точный поиск по инвертированному индексу.", ["term"], {
                 "type": "object",
                 "properties": {"term": {"type": "string"}},
                 "required": ["term"],
             }),
            ("link", "Типизированная связь subject --predicate--> object "
             "(создаёт и обратную).", ["subject_key", "predicate", "object_key"], {
                 "type": "object",
                 "properties": {"subject_key": {"type": "string"},
                                "predicate": {"type": "string"},
                                "object_key": {"type": "string"}},
                 "required": ["subject_key", "predicate", "object_key"],
             }),
            ("unlink", "Удаление связи.", ["subject_key", "predicate", "object_key"], {
                 "type": "object",
                 "properties": {"subject_key": {"type": "string"},
                                "predicate": {"type": "string"},
                                "object_key": {"type": "string"}},
                 "required": ["subject_key", "predicate", "object_key"],
             }),
            ("graph", "Обход графа связей от ключа.", ["key"], {
                 "type": "object",
                 "properties": {"key": {"type": "string"}, "depth": {"type": "integer"}},
                 "required": ["key"],
             }),
            ("vocab_add", "Добавить модуль или тему в контролируемый словарь.",
             ["module"], {
                 "type": "object",
                 "properties": {"module": {"type": "string"}, "topic": {"type": "string"}},
                 "required": ["module"],
             }),
            ("vocab_find", "Fuzzy-поиск по словарю.", ["query"], {
                 "type": "object",
                 "properties": {"query": {"type": "string"}},
                 "required": ["query"],
             }),
            ("exists", "Проверка существования записи по ключу.", ["key"], {
                 "type": "object",
                 "properties": {"key": {"type": "string"}},
                 "required": ["key"],
             }),
            ("dump_context", "Снимок контекста перед сжатием.", [], {
                 "type": "object",
                 "properties": {"task": {"type": "string"}, "milestone": {"type": "string"},
                                "keys": {"type": "array"}, "hypotheses": {"type": "array"}},
             }),
            ("restore_context", "Восстановление контекста после сжатия.", ["context_id"], {
                 "type": "object",
                 "properties": {"context_id": {"type": "string"}},
                 "required": ["context_id"],
             }),
            ("gc", "Тиринг записей hot/warm/cold (никогда не удаляет без явного purge).",
             [], {
                 "type": "object",
                 "properties": {"dry_run": {"type": "boolean"},
                                "archive": {"type": "boolean"}},
             }),
            ("purge", "Явное удаление записей и их индексов.", ["keys"], {
                 "type": "object",
                 "properties": {"keys": {"type": "array", "items": {"type": "string"}}},
                 "required": ["keys"],
             }),
            ("stats", "Статистика хранилища.", [], {"type": "object"}),
            ("db_status", "Диагностика БД по мета-страницам/bootid/txnid "
             "(живут в файле, НЕ в LCK): preopen.recent_txnid + env.meta_txnid, "
             "bootid. Критерий «достигли ли данные диска» и «откат к steady».", [],
             {"type": "object"}),
            ("db_flush", "Принудительный сброс данных на диск (sync force=true).",
             [], {"type": "object"}),
            ("db_readers", "Число активных читателей; check=True очищает мёртвые.",
             [], {"type": "object",
                  "properties": {"check": {"type": "boolean"}}}),
            ("db_stat", "Статистика env или конкретной таблицы.", [], {
                 "type": "object",
                 "properties": {"table": {"type": "string"}}}),
            ("db_set_mode", "Переключение режима БД: sync "
             "(durable|metasync|safe_nosync) на лету; readonly true/false — "
             "переоткрытие env. По умолчанию сервер стартует read-only; для "
             "записи сначала переключитесь в read-write. ВНИМАНИЕ: "
             "utterly_nosync здесь НЕдоступен — он только через отдельный "
             "опасный инструмент db_enable_utterly_nosync.",
             ["sync", "readonly"], {
                 "type": "object",
                 "properties": {"sync": {"type": "string"},
                                "readonly": {"type": "boolean"}},
             }),
            ("db_enable_utterly_nosync", "ОПАСНО: полное отключение "
             "синхронизации (MDBX_UTTERLY_NOSYNC). Только для одноразовых "
             "кэшей/некритичных данных: после краха процесса (SIGKILL, "
             "падение) данные последних транзакций могут быть потеряны без "
             "возможности восстановления. Обычный сервер памяти роя НЕ "
             "должен включать этот режим.", [], {"type": "object"}),
            ("db_backup", "Консистентная копия БД (MVCC-снапшот, компактификация). "
             "kind=user|manual; skip=true — ответ на PROMPT-запрос (пропустить). "
             "Если установлен PROMPT-таймер (SHARED_GRAPH_MEMORY_PROMPT_BACKUP_SECONDS) "
             "и агент получил notice — спросите пользователя и вызовите "
             "db_backup kind=user (сделать) или db_backup skip=true (пропустить).",
             [], {"type": "object",
                  "properties": {"kind": {"type": "string"},
                                 "dest": {"type": "string"},
                                 "skip": {"type": "boolean"}}}),
            ("db_backup_status", "Состояние бэкапов: каталог, таймеры AUTO/PROMPT, "
             "счётчики по тирам (lifeline/auto/user/snapshot), кандидаты на ротацию, "
             "лимиты.", [], {"type": "object"}),
            ("db_backup_cleanup", "Ротация AUTO-копий: показать кандидатов "
             "(dry_run=true, по умолчанию) или удалить (confirm=true). Удаляются "
             "ТОЛЬКО старейшие auto-* сверх keep; тиры user/lifeline/snapshot "
             "никогда не трогаются. Перед удалением — подтверждение пользователя.",
             [], {"type": "object",
                  "properties": {"dry_run": {"type": "boolean"},
                                 "confirm": {"type": "boolean"},
                                 "keep": {"type": "integer"}}}),
            ("db_fileinfo", "fstat(fd) файла БД: st_nlink/st_size/inode/mtime + "
             "признак удаления файла (предохранитель LIFELINE).", [], {"type": "object"}),
            ("db_latency", "Commit-латентности последних write-транзакций по стадиям "
             "(preparation/gc/write/sync/ending/whole в µs) + сводка по окну. "
             "gc_prof заполняется только в сборках с MDBX_ENABLE_PROFGC.",
             [], {"type": "object"}),
("db_recover", "Диагностика меты для восстановления (безопасный probe; "
              "open_for_recovery требует остановленного сервера). target_meta=0..2. "
              "Обычно предпочтителен mdbx_chk или восстановление из db_backup.",
              [], {"type": "object",
                   "properties": {"target_meta": {"type": "integer"}}}),
            ("map_symbol", "Символ карты исходников по fq-ключу (fn:/type:/macro:).",
             [], {"type": "object", "properties": {"key": {"type": "string"}}}),
            ("map_symbols", "Префиксный список символов карты (limit).",
             [], {"type": "object", "properties": {"prefix": {"type": "string"},
                                                  "limit": {"type": "integer"}}}),
            ("map_edges_of", "Исходящие вызовы символа (caller -> callee).",
             [], {"type": "object", "properties": {"caller": {"type": "string"}}}),
            ("map_callers_of", "Кто вызывает символ (обратные рёбра, impact).",
             [], {"type": "object", "properties": {"callee": {"type": "string"}}}),
            ("map_group_members", "Члены фасет-группы (group:{kind}:{name}).",
             [], {"type": "object", "properties": {"group": {"type": "string"}}}),
            ("map_regions", "#if-дерево: префиксный список регионов region:{module}:{n}.",
             [], {"type": "object", "properties": {"prefix": {"type": "string"},
                                                   "limit": {"type": "integer"}}}),
            ("map_region", "Один регион #if-дерева по ключу.",
             [], {"type": "object", "properties": {"key": {"type": "string"}}}),
            ("map_uncovered", "Uncovered-острова смысла (префикс модуля, limit).",
             [], {"type": "object", "properties": {"prefix": {"type": "string"},
                                                   "limit": {"type": "integer"}}}),
        ]
        return {name: {"name": name, "description": desc, "inputSchema": schema}
                for name, desc, _args, schema in t}

    def handle(self, msg: dict):
        mid = msg.get("id")
        method = msg.get("method")
        if method == "initialize":
            return self._resp(mid, {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "shared-graph-memory.mdbx", "version": "0.1.0"},
            })
        if method == "notifications/initialized":
            return None
        if method == "ping":
            return self._resp(mid, {})
        if method == "resources/list":
            return self._resp(mid, {"resources": []})
        if method == "resources/templates/list":
            return self._resp(mid, {"resourceTemplates": []})
        if method == "tools/list":
            return self._resp(mid, {"tools": list(self.tools.values())})
        if method == "tools/call":
            params = msg.get("params") or {}
            tname = params.get("name")
            args = params.get("arguments") or {}
            tool = self.tools.get(tname)
            if tool is None:
                return self._error(mid, -32602, "Unknown tool: %s" % tname)
            try:
                # предохранитель: детекция удаления файла БД + таймеры бэкапов
                notice = self.store.before_call()
                result = self._dispatch(tname, args)
                if notice:
                    result.setdefault("_notice", notice)
                text = json.dumps(result, ensure_ascii=False, default=str)
                return self._resp(mid, {
                    "content": [{"type": "text", "text": text}],
                    "structuredContent": result,
                })
            except MemoryError as e:
                # контрактная ошибка: класс + действие + политика ретрая
                return self._error(mid, -32000, str(e))
            except Exception as e:  # noqa: BLE001
                return self._error(mid, -32000, "%s: %s" % (tname, e))
        if method == "logging/setLevel":
            return self._resp(mid, {})
        return self._error(mid, -32601, "Method not found: %s" % method)

    def _dispatch(self, name: str, args: dict):
        s = self.store
        if name == "normalize_key":
            return {"canonical_key": s.norm.normalize(args["raw"])}
        if name == "safe_store":
            # importance не приводим здесь: store.safe_store сам валидирует
            # и вернёт контрактную ошибку invalid для нечислового значения
            return s.safe_store(args["key"], args["type"], args["summary"],
                                args["importance"], args.get("date", ""))
        if name == "recall":
            return {"records": s.recall(args["pattern"], args.get("limit", 5))}
        if name == "search":
            return {"records": s.search(args["query"], args.get("limit", 5))}
        if name == "lookup":
            return {"record_keys": s.lookup(args["term"])}
        if name == "link":
            return s.link(args["subject_key"], args["predicate"], args["object_key"])
        if name == "unlink":
            return s.unlink(args["subject_key"], args["predicate"], args["object_key"])
        if name == "graph":
            return s.graph(args["key"], args.get("depth", 1))
        if name == "vocab_add":
            return s.vocab_add(args["module"], args.get("topic", ""))
        if name == "vocab_find":
            return s.vocab_find(args["query"])
        if name == "exists":
            return {"exists": s.exists(args["key"])}
        if name == "dump_context":
            return s.dump_context(args.get("task", ""), args.get("milestone", ""),
                                  args.get("keys"), args.get("hypotheses"))
        if name == "restore_context":
            return {"records": s.restore_context(args["context_id"])}
        if name == "gc":
            return s.gc(bool(args.get("dry_run", True)), bool(args.get("archive", False)))
        if name == "purge":
            return s.purge(args.get("keys") or [])
        if name == "stats":
            return s.stats()
        if name == "db_status":
            return s.diag()
        if name == "db_flush":
            return s.flush_sync()
        if name == "db_readers":
            return s.readers(bool(args.get("check", False)))
        if name == "db_stat":
            return s.db_stat(args.get("table"))
        if name == "db_set_mode":
            sync = args.get("sync")
            readonly = args.get("readonly")
            result = {}
            if sync is not None:
                result.update(s.set_sync_mode(sync))
            if readonly is not None:
                result.update(s.set_readonly(bool(readonly)))
            return result
        if name == "db_enable_utterly_nosync":
            return s.enable_utterly_nosync()
        if name == "db_backup":
            if args.get("skip"):
                return s.run_prompt_backup(approve=False)
            kind = args.get("kind") or ("user" if s.backup_pending else "manual")
            if s.backup_pending:
                r = s.run_prompt_backup(approve=True)
                if r.get("approved") and "error" not in r:
                    return r
                raise MemoryError("BACKUP_FAILED", "internal",
                                  r.get("error", "бэкап не создан"),
                                  "проверьте backup_dir и права на запись", "none")
            return s.backup(kind=kind, dest=args.get("dest"))
        if name == "db_backup_status":
            return s.backup_status()
        if name == "db_backup_cleanup":
            return s.cleanup(dry_run=bool(args.get("dry_run", True)),
                             confirm=bool(args.get("confirm", False)),
                             keep=args.get("keep"))
        if name == "db_fileinfo":
            return s.fileinfo()
        if name == "db_latency":
            return s.commit_latency()
        if name == "db_recover":
            target = int(args.get("target_meta", 0))
            return s.recover(target)
        if name == "map_symbol":
            return s.map_symbol(args["key"])
        if name == "map_symbols":
            return {"symbols": s.map_symbols(args.get("prefix", ""),
                                             limit=args.get("limit"))}
        if name == "map_edges_of":
            return {"edges": s.map_edges_of(args["caller"])}
        if name == "map_callers_of":
            return {"callers": s.map_callers_of(args["callee"])}
        if name == "map_group_members":
            return {"members": s.map_group_members(args["group"])}
        if name == "map_regions":
            return {"regions": s.map_regions(args.get("prefix", ""),
                                             limit=args.get("limit"))}
        if name == "map_region":
            return s.map_region(args["key"])
        if name == "map_uncovered":
            return {"uncovered": s.map_uncovered(args.get("prefix", ""),
                                                 limit=args.get("limit"))}
        raise MemoryError("NO_SUCH_TOOL", "invalid", "неизвестный инструмент %r" % name,
                          "проверьте tools/list", "none")

    @staticmethod
    def _resp(mid, result):
        out = {"jsonrpc": "2.0", "result": result}
        if mid is not None:
            out["id"] = mid
        return out

    @staticmethod
    def _error(mid, code, message):
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}}

    def loop(self, stdin=None, stdout=None):
        stdin = stdin if stdin is not None else sys.stdin.buffer
        stdout = stdout if stdout is not None else sys.stdout.buffer
        for raw in stdin:
            line = raw.decode("utf-8", "replace").strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            resp = self.handle(msg)
            if resp is not None:
                stdout.write((json.dumps(resp, ensure_ascii=False) + "\n").encode("utf-8"))
                stdout.flush()


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    path = _default_db_path()
    readonly = os.environ.get("SHARED_GRAPH_MEMORY_READONLY", "1") not in ("0", "false", "")
    created = False
    # БД ещё не существует — создаём (readonly старт бессмыслен без файла);
    # иначе открываем в read-only и переключаемся в rw по запросу агента.
    if readonly and not os.path.exists(path):
        Store(path, readonly=False).close()
        created = True
    try:
        store = Store(path, readonly=readonly)
    except mdbx.LibmdbxError as e:
        if e.rc in (mdbx.RC_WANNA_RECOVERY, mdbx.RC_CORRUPTED):
            print("shared-graph-memory: DB требует recovery (аварийное "
                  "завершение без close, меты unsteady):", file=sys.stderr)
            print("  %s" % e, file=sys.stderr)
            print("  Варианты:", file=sys.stderr)
            print("    1) запустить mdbx_chk для проверки/ремонта;", file=sys.stderr)
            print("    2) осознанно открыть read-write "
                  "(SHARED_GRAPH_MEMORY_READONLY=0) — движок сделает steady-sync;",
                  file=sys.stderr)
            print("    3) восстановить из бэкапа (см. db_backup / backup_dir).",
                  file=sys.stderr)
            return 2
        raise
    try:
        diag = store.env.diag()
    except Exception:  # noqa: BLE001
        diag = {}
    print("shared-graph-memory started: path=%s readonly=%s sync_mode=%s "
          "next_id=%s meta_txnid=%s%s" % (
              path, readonly, store.sync_mode, store.next_id,
              diag.get("meta_txnid"), " CREATED-NEW" if created else ""),
          file=sys.stderr, flush=True)

    def _shutdown(signum=None, frame=None):
        try:
            store.flush_sync()
        except Exception:
            pass
        store.close()
        sys.exit(0)

    try:
        import signal
        for sig in (signal.SIGTERM, signal.SIGINT):
            try:
                signal.signal(sig, _shutdown)
            except (ValueError, OSError):
                pass
        server = McpServer(store)
        server.loop()
    finally:
        store.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())