"""MCP-сервер памяти роя (JSON-RPC 2.0 поверх stdio).

Транспорт ошибок: контрактные ``error$CODE | CLASS=... | DESC=... | ACTION=...
| RETRY=...`` возвращаются как JSON-RPC protocol error (-32000), т.к. это
единственный гарантированный способ донести ошибку до модели в opencode.
"""

from __future__ import annotations

import json
import os
import sys
import traceback

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
             [], {"type": "object", "properties": {"key": {"type": "string"},
                                                   "layer": {"type": "string"}}}),
            ("map_symbols", "Префиксный список символов карты (limit, layer).",
             [], {"type": "object", "properties": {"prefix": {"type": "string"},
                                                  "limit": {"type": "integer"},
                                                  "layer": {"type": "string"}}}),
            ("map_edges_of", "Исходящие вызовы символа (caller -> callee; "
             "принимает caller|symbol, layer).", [], {"type": "object",
             "properties": {"caller": {"type": "string"},
                            "symbol": {"type": "string"},
                            "layer": {"type": "string"}}}),
            ("map_callers_of", "Кто вызывает символ (callee|symbol, layer; impact).",
             [], {"type": "object",
                  "properties": {"callee": {"type": "string"},
                                 "symbol": {"type": "string"},
                                 "layer": {"type": "string"}}}),
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
            ("test_metrics_insert", "Зафиксировать длительность тест-кейса "
             "(скользящее окно истории).", ["test"], {
                 "type": "object",
                 "properties": {"test": {"type": "string"},
                                "duration_ms": {"type": "number"},
                                "labels": {"type": "array"},
                                "timeout_sec": {"type": "integer"}},
                 "required": ["test", "duration_ms"]}),
            ("test_metrics_get", "Окно истории длительностей тест-кейса.",
             ["test"], {"type": "object",
                        "properties": {"test": {"type": "string"}},
                        "required": ["test"]}),
            ("test_metrics_all", "Все метрики тестов (prefix, limit).",
             [], {"type": "object",
                  "properties": {"prefix": {"type": "string"},
                                 "limit": {"type": "integer"}}}),
            ("coverage_insert", "Зафиксировать покрытие теста (набор символов).",
             ["test"], {"type": "object",
                        "properties": {"test": {"type": "string"},
                                       "symbols": {"type": "array"},
                                       "pct": {"type": "number"}},
                        "required": ["test", "symbols"]}),
            ("coverage_get", "Покрытие конкретного теста.", ["test"],
             {"type": "object",
              "properties": {"test": {"type": "string"}},
              "required": ["test"]}),
            ("coverage_all", "Все покрытия (limit).", [],
             {"type": "object",
              "properties": {"limit": {"type": "integer"}}}),
            ("coverage_batch", "Батчевая фиксация покрытия {test: {symbols, pct}}.",
             ["tests"], {"type": "object",
                         "properties": {"tests": {"type": "object"}},
                         "required": ["tests"]}),
            ("coverage_symbols", "Тесты, покрывающие символы (prefix).",
             [], {"type": "object",
                  "properties": {"symbol_prefix": {"type": "string"},
                                 "limit": {"type": "integer"}}}),
            ("coverage_remove", "Удалить покрытие теста + covered_by.", ["test"],
             {"type": "object",
              "properties": {"test": {"type": "string"}},
              "required": ["test"]}),
            ("coverage_blocks_insert", "Сохранить блочную детализацию "
             "покрытия теста (blocks).", ["test", "blocks"], {
                 "type": "object",
                 "properties": {"test": {"type": "string"},
                                "blocks": {"type": "array"},
                                "layer": {"type": "string"}},
                 "required": ["test", "blocks"]}),
            ("coverage_blocks_get", "Блоки символа, покрытые тестом.",
             ["test", "symbol"], {"type": "object",
                                  "properties": {"test": {"type": "string"},
                                                 "symbol": {"type": "string"},
                                                 "layer": {"type": "string"}},
                                  "required": ["test", "symbol"]}),
            ("coverage_gaps", "Белые пятна: символы module без покрытия.",
             [], {"type": "object",
                  "properties": {"module": {"type": "string"},
                                 "layer": {"type": "string"},
                                 "limit": {"type": "integer"}}}),
            ("ci_jobs_insert", "Зафиксировать параметры CI-джобы "
             "(\"{workflow}/{job}\").", ["job"], {
                 "type": "object",
                 "properties": {"job": {"type": "string"},
                                "body": {"type": "object"}},
                 "required": ["job"]}),
            ("ci_jobs_all", "Все CI-джобы (prefix, limit).", [],
             {"type": "object",
              "properties": {"prefix": {"type": "string"},
                             "limit": {"type": "integer"}}}),
            ("ci_jobs_get", "Параметры одной CI-джобы.", ["job"],
             {"type": "object",
              "properties": {"job": {"type": "string"}},
              "required": ["job"]}),
            ("ci_jobs_by_test", "Джобы, исполняющие тест.", ["test"],
             {"type": "object",
              "properties": {"test": {"type": "string"}},
              "required": ["test"]}),
            ("project_load", "Загрузить project-map (files+file_deps).",
             ["files"], {"type": "object",
                         "properties": {"files": {"type": "object"},
                                        "edges": {"type": "array"},
                                        "replace": {"type": "boolean"}},
                         "required": ["files"]}),
            ("project_files", "Файлы проекта по префиксу пути.", [],
             {"type": "object",
              "properties": {"prefix": {"type": "string"},
                             "limit": {"type": "integer"}}}),
            ("file_deps", "Зависимости файла (file:path).", ["path"],
             {"type": "object",
              "properties": {"path": {"type": "string"}},
              "required": ["path"]}),
            ("file_dependents", "Файлы, зависящие от path.", ["path"],
             {"type": "object",
              "properties": {"path": {"type": "string"},
                             "limit": {"type": "integer"}},
              "required": ["path"]}),
            ("probe_add", "Регистрация зонда (SystemMap), dedup по тегу.", ["tag"],
             {"type": "object",
              "properties": {"tag": {"type": "string"},
                             "kind": {"type": "string"},
                             "mechanism": {"type": "string"},
                             "test_theme": {"type": "string"},
                             "symbol": {"type": "string"},
                             "file": {"type": "string"},
                             "line": {"type": "integer"},
                             "status": {"type": "string"}},
              "required": ["tag"]}),
            ("probe_batch", "Батчевая регистрация зондов (scan_probes.py).",
             ["probes"], {"type": "object",
                          "properties": {"probes": {"type": "array"}},
                          "required": ["probes"]}),
            ("probe_get", "Зонд по тегу.", ["tag"],
             {"type": "object",
              "properties": {"tag": {"type": "string"}},
              "required": ["tag"]}),
            ("probe_list", "Зонды по префиксу/kind.", [],
             {"type": "object",
              "properties": {"prefix": {"type": "string"},
                             "kind": {"type": "string"},
                             "limit": {"type": "integer"}}}),
            ("probe_status", "Сменить статус зонда.", ["tag", "status"],
             {"type": "object",
              "properties": {"tag": {"type": "string"},
                             "status": {"type": "string"}},
              "required": ["tag", "status"]}),
            ("probe_remove", "Удалить зонд.", ["tag"],
             {"type": "object",
              "properties": {"tag": {"type": "string"}},
              "required": ["tag"]}),
            ("trace_ingest", "Наблюдаемый вызов (runtime): caller → callee.",
             ["caller", "callee"], {"type": "object",
                                    "properties": {"caller": {"type": "string"},
                                                   "callee": {"type": "string"},
                                                   "count": {"type": "integer"},
                                                   "label": {"type": "string"}},
                                    "required": ["caller", "callee"]}),
            ("trace_observed", "Наблюдаемые вызовы (prefix caller).", [],
             {"type": "object",
              "properties": {"caller": {"type": "string"}}}),
            ("trace_path", "Пути по observed-рёбрам src → dst.",
             ["src", "dst"], {"type": "object",
                              "properties": {"src": {"type": "string"},
                                             "dst": {"type": "string"},
                                             "depth": {"type": "integer"},
                                             "limit": {"type": "integer"}},
                              "required": ["src", "dst"]}),
            ("trace_compare", "Compare static (call_edges) vs observed (traces).",
             ["symbol"], {"type": "object",
                          "properties": {"symbol": {"type": "string"}},
                          "required": ["symbol"]}),
            ("map_put_edge", "Записать ребро вызова (kind: syntax|semantic).",
             ["caller", "callee"], {"type": "object",
                                    "properties": {"caller": {"type": "string"},
                                                   "callee": {"type": "string"},
                                                   "kind": {"type": "string"},
                                                   "layer": {"type": "string"},
                                                   "configs": {"type": "array"}},
                                    "required": ["caller", "callee"]}),
            ("map_mark_stale", "Пометить отсутствующие в live_keys символы "
             "как stale (alive остальным).", ["live_keys"], {
                 "type": "object",
                 "properties": {"live_keys": {"type": "array"}},
                 "required": ["live_keys"]}),
            ("map_generations", "Поколение карты (canary.y) + счётчики.", [],
             {"type": "object"}),
            ("map_snapshot", "Полный снимок карты (для diff поколений).", [],
             {"type": "object"}),
            ("map_diff", "Разница текущего снимка с other (keys/generation).",
             [], {"type": "object",
                  "properties": {"other": {"type": "object"}}}),
            ("adr_update", "Сохранить/обновить ADR (context/decision/consequences).",
             ["name"], {"type": "object",
                        "properties": {"name": {"type": "string"},
                                       "title": {"type": "string"},
                                       "status": {"type": "string"},
                                       "date": {"type": "string"},
                                       "context": {"type": "string"},
                                       "decision": {"type": "string"},
                                       "consequences": {"type": "string"},
                                       "sections": {"type": "object"}},
                        "required": ["name"]}),
            ("adr_get", "Получить ADR по имени.", ["name"],
             {"type": "object",
              "properties": {"name": {"type": "string"}},
              "required": ["name"]}),
            ("adr_outline", "Обзор всех ADR (title/status/date).", [],
             {"type": "object",
              "properties": {"prefix": {"type": "string"},
                             "limit": {"type": "integer"}}}),
            ("adr_delete", "Удалить ADR.", ["name"],
             {"type": "object",
              "properties": {"name": {"type": "string"}},
              "required": ["name"]}),
            ("cbm_link", "Мост: наш fq-ключ ↔ узел codebase-memory-mcp.",
             ["our_key", "cbm_node"], {"type": "object",
                                       "properties": {"our_key": {"type": "string"},
                                                      "cbm_node": {"type": "string"},
                                                      "cbm_project": {"type": "string"}},
                                       "required": ["our_key", "cbm_node"]}),
            ("cbm_unlink", "Убрать мост для нашего ключа.", ["our_key"],
             {"type": "object",
              "properties": {"our_key": {"type": "string"}},
              "required": ["our_key"]}),
            ("cbm_linked", "Список мостов по префиксу.", [],
             {"type": "object",
              "properties": {"prefix": {"type": "string"},
                             "limit": {"type": "integer"}}}),
            ("cbm_search", "Поиск в codebase-memory-mcp (CLI, опционально).",
             ["query"], {"type": "object",
                         "properties": {"query": {"type": "string"},
                                        "project": {"type": "string"},
                                        "tool": {"type": "string"},
                                        "args": {"type": "object"}},
                         "required": ["query"]}),
            ("version_info", "Версии: модуль / libmdbx / схема данных / данные.",
             [], {"type": "object"}),
            ("branch_register", "Регистрация слоя ветки "
             "({ver}.{feature}.{n}).", ["ver_slug"], {
                 "type": "object",
                 "properties": {"ver_slug": {"type": "string"},
                                "feature_slug": {"type": "string"},
                                "n_commits": {"type": "integer"},
                                "revision": {"type": "string"},
                                "git_tag": {"type": "string"},
                                "base_branch": {"type": "string"},
                                "status": {"type": "string"}},
                 "required": ["ver_slug"]}),
            ("branch_list", "Список слоёв веток (base_only?).", [],
             {"type": "object",
              "properties": {"base_only": {"type": "boolean"}}}),
            ("branch_current", "Активный слой.", [], {"type": "object"}),
            ("branch_activate", "Активировать слой.", ["layer"],
             {"type": "object",
              "properties": {"layer": {"type": "string"}},
              "required": ["layer"]}),
            ("branch_remove", "Убрать слой ветки.", ["layer"],
             {"type": "object",
              "properties": {"layer": {"type": "string"}},
              "required": ["layer"]}),
            ("map_diff_branches", "Структурный diff двух слоёв (по содержимому).",
             ["from_layer", "to_layer"], {"type": "object",
                                          "properties": {"from_layer": {"type": "string"},
                                                         "to_layer": {"type": "string"},
                                                         "limit": {"type": "integer"}},
                                          "required": ["from_layer", "to_layer"]}),
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
                # внутренний сбой: логируем полный traceback (в stderr) для
                # диагностики, а агенту отдаём контрактную ошибку
                traceback.print_exc(file=sys.stderr)
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
            return s.map_symbol(args["key"], layer=args.get("layer", ""))
        if name == "map_symbols":
            return {"symbols": s.map_symbols(args.get("prefix", ""),
                                             limit=args.get("limit"),
                                             layer=args.get("layer", ""))}
        if name == "map_edges_of":
            # алиас: агент может передать caller или symbol (семант. имя)
            sym = args.get("caller") or args.get("symbol")
            if not sym:
                raise MemoryError(
                    "invalid", "invalid", "map_edges_of требует 'caller' или 'symbol'",
                    "укажите символ", "none")
            return {"edges": s.map_edges_of(sym, layer=args.get("layer", ""))}
        if name == "map_callers_of":
            sym = args.get("callee") or args.get("symbol")
            if not sym:
                raise MemoryError(
                    "invalid", "invalid", "map_callers_of требует 'callee' или 'symbol'",
                    "укажите символ", "none")
            return {"callers": s.map_callers_of(sym, layer=args.get("layer", ""))}
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
        if name == "test_metrics_insert":
            return s.test_metrics_insert(args["test"],
                                         float(args["duration_ms"]),
                                         labels=args.get("labels"),
                                         timeout_sec=args.get("timeout_sec", 0))
        if name == "test_metrics_get":
            return s.test_metrics_get(args["test"])
        if name == "test_metrics_all":
            return {"tests": s.test_metrics_all(args.get("prefix", ""),
                                                limit=args.get("limit"))}
        if name == "coverage_insert":
            return s.coverage_insert(args["test"], args.get("symbols") or [],
                                     float(args.get("pct", 0.0)))
        if name == "coverage_get":
            return s.coverage_get(args["test"])
        if name == "coverage_all":
            return {"coverage": s.coverage_all(limit=args.get("limit"))}
        if name == "coverage_batch":
            return s.coverage_batch(args.get("tests") or {})
        if name == "coverage_symbols":
            return {"symbols": s.coverage_symbols(
                args.get("symbol_prefix", ""), limit=args.get("limit"))}
        if name == "coverage_remove":
            return s.coverage_remove(args["test"])
        if name == "coverage_blocks_insert":
            return {"stored": s.coverage_blocks_insert(
                args["test"], args.get("blocks") or [],
                layer=args.get("layer", ""))}
        if name == "coverage_blocks_get":
            return {"blocks": s.coverage_blocks_get(
                args["test"], args["symbol"], layer=args.get("layer", ""))}
        if name == "coverage_gaps":
            return s.coverage_gaps(module=args.get("module", ""),
                                   layer=args.get("layer", ""),
                                   limit=args.get("limit"))
        if name == "ci_jobs_insert":
            return s.ci_jobs_insert(args["job"], args.get("body") or {})
        if name == "ci_jobs_all":
            return {"jobs": s.ci_jobs_all(args.get("prefix", ""),
                                          limit=args.get("limit"))}
        if name == "ci_jobs_get":
            return s.ci_jobs_get(args["job"])
        if name == "ci_jobs_by_test":
            return {"jobs": s.ci_jobs_by_test(args["test"])}
        if name == "project_load":
            return s.project_load(args.get("files") or {},
                                  args.get("edges") or [],
                                  replace=bool(args.get("replace", True)))
        if name == "project_files":
            return {"files": s.project_files(args.get("prefix", ""),
                                             limit=args.get("limit"))}
        if name == "file_deps":
            return {"deps": s.file_deps(args["path"])}
        if name == "file_dependents":
            return {"dependents": s.file_dependents(
                args["path"], limit=args.get("limit"))}
        if name == "probe_add":
            # kind/status передаются только если указаны (None => merge сохранит старое)
            return s.probe_add(args["tag"], kind=args.get("kind"),
                               mechanism=args.get("mechanism", ""),
                               test_theme=args.get("test_theme", ""),
                               symbol=args.get("symbol", ""),
                               file=args.get("file", ""),
                               line=int(args.get("line", 0)),
                               status=args.get("status"))
        if name == "probe_batch":
            return s.probe_batch(args.get("probes") or [])
        if name == "probe_get":
            return s.probe_get(args["tag"])
        if name == "probe_list":
            return {"probes": s.probe_list(args.get("prefix", ""),
                                           kind=args.get("kind", ""),
                                           limit=args.get("limit"))}
        if name == "probe_status":
            return s.probe_status(args["tag"], args["status"])
        if name == "probe_remove":
            return s.probe_remove(args["tag"])
        if name == "trace_ingest":
            return s.trace_ingest(args["caller"], args["callee"],
                                  count=int(args.get("count", 1)),
                                  label=args.get("label", ""))
        if name == "trace_observed":
            return {"traces": s.trace_observed(args.get("caller", ""))}
        if name == "trace_path":
            return {"paths": s.trace_path(args["src"], args["dst"],
                                          depth=int(args.get("depth", 5)),
                                          limit=args.get("limit", 50))}
        if name == "trace_compare":
            return s.trace_compare(args["symbol"])
        if name == "map_put_edge":
            s.map_put_edge(args["caller"], args["callee"],
                           kind=args.get("kind", "syntax"),
                           layer=args.get("layer", ""),
                           configs=args.get("configs"))
            return {"result": "ok"}
        if name == "map_mark_stale":
            return s.map_mark_stale(args.get("live_keys") or [])
        if name == "map_generations":
            return s.map_generations()
        if name == "map_snapshot":
            return s.map_snapshot()
        if name == "map_diff":
            return s.map_diff(other=args.get("other"))
        if name == "adr_update":
            return s.adr_update(args["name"], args)
        if name == "adr_get":
            return s.adr_get(args["name"])
        if name == "adr_outline":
            return {"adr": s.adr_outline(args.get("prefix", ""),
                                         limit=args.get("limit"))}
        if name == "adr_delete":
            return s.adr_delete(args["name"])
        if name == "cbm_link":
            return s.cbm_link(args["our_key"], args["cbm_node"],
                              cbm_project=args.get("cbm_project", ""))
        if name == "cbm_unlink":
            return s.cbm_unlink(args["our_key"])
        if name == "cbm_linked":
            return {"bridges": s.cbm_linked(args.get("prefix", ""),
                                            limit=args.get("limit"))}
        if name == "cbm_search":
            return s.cbm_search(args["query"],
                                project=args.get("project", ""),
                                tool=args.get("tool", "search_graph"),
                                args=args.get("args"))
        if name == "version_info":
            return s.version_info()
        if name == "branch_register":
            return s.branch_register(
                args["ver_slug"], feature_slug=args.get("feature_slug", "base"),
                n_commits=int(args.get("n_commits", 0)),
                revision=args.get("revision", ""),
                git_tag=args.get("git_tag", ""),
                base_branch=args.get("base_branch", ""),
                status=args.get("status", "active"))
        if name == "branch_list":
            return {"branches": s.branch_list(base_only=bool(args.get("base_only")))}
        if name == "branch_current":
            return s.branch_current()
        if name == "branch_activate":
            return s.branch_activate(args["layer"])
        if name == "branch_remove":
            return s.branch_remove(args["layer"])
        if name == "map_diff_branches":
            return s.map_diff_branches(args["from_layer"], args["to_layer"],
                                       limit=args.get("limit"))
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
    elif not readonly:
        # rw-старт: создаём/обновляем схему (новые таблицы для старых БД);
        # временный Store ЗАКРЫВАЕМ до основного открытия
        with Store(path, readonly=False) as _upg:
            _upg.upgrade()
    elif os.path.exists(path):
        # readonly-старт существующей БД: upgrade создаёт недостающие таблицы,
        # НО только если БД не требует recovery (unsteady): preopen-проверка
        # не пишет в БД и сохраняет recovery-сигнал для обработки ниже.
        try:
            mdbx.preopen_snapinfo(path)
            with Store(path, readonly=False) as _upg:
                _upg.upgrade()
        except mdbx.LibmdbxError as e:
            if e.rc in (mdbx.RC_WANNA_RECOVERY, mdbx.RC_CORRUPTED):
                pass  # recovery-путь ниже (rc=2)
            else:
                raise
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