"""MCP-сервер памяти роя (JSON-RPC 2.0 поверх stdio).

Транспорт ошибок: контрактные ``error$CODE | CLASS=... | DESC=... | ACTION=...
| RETRY=...`` возвращаются как JSON-RPC protocol error (-32000), т.к. это
единственный гарантированный способ донести ошибку до модели в opencode.
"""

from __future__ import annotations

import json
import os
import sys

from .errors import MemoryError
from .store import Store


def _default_db_path() -> str:
    env = os.environ.get("MEMORY_MDBX_PATH")
    if env:
        return env
    return os.path.join(os.path.expanduser("~"), ".local", "share",
                        "mcp-memory", "shared-graph-memory.mdbx")


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
                "serverInfo": {"name": "mcp-memory", "version": "0.1.0"},
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
                result = self._dispatch(tname, args)
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
            return s.safe_store(args["key"], args["type"], args["summary"],
                                float(args["importance"]), args.get("date", ""))
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
    store = Store(path)
    try:
        server = McpServer(store)
        server.loop()
    finally:
        store.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())