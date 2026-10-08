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
| `mcp/libmdbx.py` | Низкоуровневый cffi-биндинг (MVal удерживает буферы iov_base) |
| `mcp/errors.py` | Контракт ошибок `error$CODE \| CLASS \| DESC \| ACTION \| RETRY` |
| `mcp/normalize.py` | Канонизация ключей `тип:модуль:тема` + контролируемый словарь |
| `mcp/index.py` | Токенизация + SimHash (64-бит) для дедупликации |
| `mcp/store.py` | Store: 11 таблиц, все операции, одна write-txn на операцию |
| `mcp/mcp_server.py` | MCP-сервер (JSON-RPC 2.0 над stdio) |
| `mcp/cli.py` | memory-cli (dump/stats/graph/audit/gc/vocab/purge) |
| `tests/` | 168 тестов, покрытие 99% |

## Требования

- Python ≥ 3.9, `cffi`, `pytest` (для тестов).
- `libmdbx`: собирается из master-дерева этого же репозитория через CMake
  (см. «Сборка из master»), результат кладётся в `mcp/_lib/` и находится
  автоматически. Порядок поиска: env `MDBX_SO_PATH` → `mcp/_lib/` →
  legacy-дефолт → системная библиотека.

## Сборка из master (основной способ)

Модуль живёт внутри репозитория libmdbx (`skynet/MCP-shared-graph-memory/`),
поэтому исходники для сборки берутся из того же дерева — дополнительных копий
не нужно. Если модуль используется отдельно — укажите корень репо через
`LIBMDBX_SRC_DIR` или `--src`.

Интеграция с основным CMake-проектом (dev-only, не попадает в амальгамат):

```bash
cmake -S . -B build -DMDBX_BUILD_MCP_MEMORY=ON        # + прочие опции проекта
ctest --test-dir build -L mcp-shared-graph-memory --output-on-failure
```

- Опция `MDBX_BUILD_MCP_MEMORY` — default OFF.
- `mcp_prepare` собирает host-libmdbx в `_build-mcp-shared-graph-memory/` (без
  CXX/TESTS/LTO) и кладёт артефакт в `mcp/_lib/`.
- Тесты `mcp_pytest` запускаются только если Python найден **и** в
  окружении хоста можно выполнять тесты (`MDBX_CAN_RUN_HOST_TESTS`; на
  cross-compile без эмулятора — пропускаются).
- Альтернатива без CMake: `python3 tools/build_libmdbx.py && python3 -m pytest tests/`.

## Запуск

```bash
SHARED_GRAPH_MEMORY_PATH=~/mem.mdbx python3 -m mcp        # MCP-сервер (stdio)
python3 -m mcp.cli --path ~/mem.mdbx stats        # CLI
python3 -m mcp.cli --path ~/mem.mdbx gc --dry-run # тиринг hot/warm/cold
```

Интеграция с opencode (`~/.config/opencode/opencode.json`):

```json
"shared-graph-memory.mdbx": {
  "type": "local",
  "command": ["python3", "/home/user/.local/share/shared-graph-memory/server.py"],
  "environment": {"SHARED_GRAPH_MEMORY_PATH": "/home/user/.local/share/shared-graph-memory/db.mdbx"}
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