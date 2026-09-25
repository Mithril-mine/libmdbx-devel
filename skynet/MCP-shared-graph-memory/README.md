# MCP-shared-graph-memory — персистентная память роя на libmdbx

Общая память агентов роя (knowledge graph): решения, баги, процессы, узкие места
и факты о проекте. Хранит **устойчивые результаты**, не промежуточные рассуждения.

- Движок: [libmdbx](https://libmdbx.dqdkfa.ru) (ACID, MVCC, single-writer).
- Реализация: Python 3 + cffi (ABI-режим).
- Документация: [`SKILL.md`](SKILL.md) (использование агентом),
  [`docs/SCHEMA.md`](docs/SCHEMA.md) (схема DBI и форматы),
  [`docs/DESIGN.md`](docs/DESIGN.md) (постановка и проектирование),
  [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (архитектура реализации).

## Структура

| Путь | Назначение |
|---|---|
| `mcp_memory/libmdbx.py` | Низкоуровневый cffi-биндинг (MVal удерживает буферы iov_base) |
| `mcp_memory/errors.py` | Контракт ошибок `error$CODE \| CLASS \| DESC \| ACTION \| RETRY` |
| `mcp_memory/normalize.py` | Канонизация ключей `тип:модуль:тема` + контролируемый словарь |
| `mcp_memory/index.py` | Токенизация + SimHash (64-бит) для дедупликации |
| `mcp_memory/store.py` | Store: 11 таблиц, все операции, одна write-txn на операцию |
| `mcp_memory/mcp_server.py` | MCP-сервер (JSON-RPC 2.0 над stdio) |
| `mcp_memory/cli.py` | memory-cli (dump/stats/graph/audit/gc/vocab/purge) |
| `tests/` | 168 тестов, покрытие 99% |

## Требования

- Python ≥ 3.9, `cffi`, `pytest` (для тестов).
- `libmdbx.so`: путь через env `MDBX_SO_PATH` (иначе дефолт
  `~/.local/share/libmdbx-memory/build/libmdbx.so`, затем системная библиотека).

## Запуск

```bash
MEMORY_MDBX_PATH=~/mem.mdbx python3 -m mcp_memory        # MCP-сервер (stdio)
python3 -m mcp_memory.cli --path ~/mem.mdbx stats        # CLI
python3 -m mcp_memory.cli --path ~/mem.mdbx gc --dry-run # тиринг hot/warm/cold
```

Интеграция с opencode (`~/.config/opencode/opencode.json`):

```json
"mcp.memory": {
  "command": "python3",
  "args": ["/abs/path/to/mcp_memory/__main__.py"],
  "env": {"MEMORY_MDBX_PATH": "/abs/path/shared-graph-memory.mdbx", "MDBX_SO_PATH": "/abs/path/libmdbx.so"}
}
```

## Тесты

```bash
nice -n 10 python3 -m pytest tests/
nice -n 10 python3 -m coverage run -m pytest tests/ && python3 -m coverage report
```

Уроки реализации (см. `docs/ARCHITECTURE.md`):
- cffi ABI-режим: enum и `#define` не создают атрибутов `ffi.*` — только числа.
- `ffi.new("void *")` запрещён — out-параметры через `void **`.
- Буферы `MDBX_val.iov_base` освобождаются GC — удерживать через `MVal`.
- Не открывать вложенные read-txn в одном потоке (`MDBX_BAD_RSLOT`) — передавать txn.