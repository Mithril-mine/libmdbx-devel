# ARCHITECTURE — архитектура реализации MCP-shared-graph-memory

## 1. Слои

```
┌─────────────────────────────────────────────┐
│ MCP-сервер (mcp_server.py)  JSON-RPC 2.0     │   tools/list, tools/call, initialize
│ CLI (cli.py)               memory-cli        │   dump/stats/graph/gc/vocab/purge
├─────────────────────────────────────────────┤
│ Store (store.py)                            │   операции, 11 DBI, 1 write-txn/операция
├─────────────────────────────────────────────┤
│ libmdbx.py (cffi ABI)                       │   MVal, Env/Txn/Cursor, константы
├─────────────────────────────────────────────┤
│ libmdbx.so (движок)                         │
└─────────────────────────────────────────────┘
```

Служебные модули: `errors.py` (контракт ошибок), `normalize.py` (ключи+словарь),
`index.py` (токенизация + SimHash).

## 2. Модель транзакций

- **Чтение**: короткие read-only txn (`MDBX_TXN_RDONLY`). Вложенные read-txn в
  одном потоке запрещены — при их открытии движок возвращает
  `MDBX_BAD_RSLOT`; поэтому вспомогательные функции (`_score_of`,
  `_incoming_count`) принимают активный `txn`, а полные обходы читают данные в
  память (`_all_records()`).
- **Запись**: одна write-txn на операцию (`_begin_write`). Конкуренция между
  процессами сериализуется single-writer моделью; `MDBX_BUSY` ретраится до 3 раз
  с задержкой 50/100/150 мс, затем поднимается контрактная ошибка `busy-io`.
- `safe_store` = один write-txn: normalize → проверка существования →
  merge (old → `history`) или SimHash-дедуп (`conflict`) → `records` +
  `ids` + `id2key` + `inverted`(термы) + `simhash` + `access` + `meta.next_id`.

## 3. Контракт ошибок

`MemoryError` сериализуется как
`error$<CODE> | CLASS=<class> | DESC=<human> | ACTION=<hint> | RETRY=<policy>`.
MCP-сервер отдаёт его как **JSON-RPC protocol error (-32000)** — единственный
гарантированный транспорт ошибки до модели в opencode (поддержка `isError`
в бинарнике не подтверждена). Классы: `invalid`, `size-limit`, `busy-io`,
`internal`. `ACTION` — рекомендация агенту.

## 4. cffi-ловушки (проверено на практике)

| Проблема | Решение |
|---|---|
| enum и `#define` не дают атрибутов `ffi.*` в ABI-режиме | только числовые литералы в Python-константах (значения сверены с mdbx.h) |
| `ffi.new("void *")` запрещён | out-параметры через `ffi.new("void **")` |
| буферы `MDBX_val.iov_base` освобождаются GC (dangling pointer → мусор в ключах) | класс `MVal` держит `_buf` живым рядом с `ptr` |
| `ffi.new("MDBX_env *")` нельзя (opaque struct) | все handle'ы — `void*` |
| вложенные read-txn → `MDBX_BAD_RSLOT` | не открывать txn внутри txn |

## 5. Индексы

- `inverted`: term → `set<uint64>` (`DUPSORT|DUPFIXED|INTEGERDUP`).
  Обновление — точечное: `NODUPDATA`-вставка, `mdbx_del(term,id)` удаление,
  при merge — diff старых/новых термов из `_terms` в теле записи.
- `links`: `{sid}\0pred` → `set<oid>`; обратная связь — `{oid}\0back:pred` → sid.
  `graph()` BFS с дедупликацией рёбер (обратные рёбра не дублируются).
  `purge` удаляет все рёбра узла (по субъекту — префиксным диапазоном, по
  объекту — сканом значений).
- `simhash`: id → 64-бит хэш; конфликт-дедуп — скан (корпус мал), Hamming ≤ 3.
- `access`: id → 24 байта `double score + uint64 last_access + uint64 count`.
  Чтения обновляют LRU с rate-limit 60 с (`_bump_access`), `touch()` — принудительно.

## 6. Персистентность словаря

`vocab` хранит `module:{m}` и `{m}:{topic}`. При старте `Store` загружает словарь
в `Normalizer`. `vocab_add(module, topic)` всегда персистит и модуль, и тему
(иначе после перезапуска модуль теряется).

## 7. gc и purge

- `gc(dry_run=true)`: классификация по композитному score, всегда возвращает
  hot/warm/cold; **никогда не удаляет**.
- `gc(archive=true)`: cold → `archive` DBI, из `records` и индексов удаляются.
- `purge(keys)`: явное удаление записей со всеми индексами и связями.
- `proc:memory:context-snapshot` всегда помечается hot (не кандидат на архив).

## 8. Тестирование

- Прямые вызовы Store без MCP (фикстура создаёт env во временном каталоге);
  MCP-сервер тестируется через `handle()` и `loop(stdin/stdout)` с инъекцией
  потоков; CLI — прямыми вызовами `main()` и подпроцессами.
- 168 тестов, покрытие 99% (непокрытое — entry-pointы `__main__` и недостижимые
  защитные ветки).
- Прогон: `nice -n 10 python3 -m pytest tests/`.

## 9. Развёртывание

### 9.1 Сборка libmdbx (интеграция с основным CMake)

Модуль живёт внутри репозитория libmdbx (`skynet/MCP-shared-graph-memory/`), поэтому
исходники для сборки — это само master-дерево рядом (никаких копий/субмодулей).
Интеграция выполнена в корневом `CMakeLists.txt` **строго внутри dist-cutoff-региона**
(не попадает в амальгамат):

```cmake
option(MDBX_BUILD_MCP_MEMORY "Build shared-graph-memory Python module (dev-only)" OFF)
# MDBX_CAN_RUN_HOST_TESTS = NOT (CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
if(MDBX_BUILD_MCP_MEMORY AND NOT MDBX_AMALGAMATED_SOURCE AND NOT_SUBPROJECT)
  add_subdirectory(skynet/MCP-shared-graph-memory)
endif()
```

- `tools/build_libmdbx.py` собирает host-libmdbx независимо (отдельный
  `_build-mcp-shared-graph-memory/`, `MDBX_BUILD_CXX=OFF`, `MDBX_ENABLE_TESTS=OFF`,
  `MDBX_BUILD_SHARED_LIBRARY=ON`, LTO=OFF, ccache) и кладёт артефакт в
  `mcp/_lib/`.
- CTest-фикстура `mcp_fixture` готовит библиотеку, тест
  `mcp_pytest` (LABELS `mcp-shared-graph-memory`) запускает pytest; оба выполняются
  только при `Python3_FOUND AND MDBX_CAN_RUN_HOST_TESTS`.
- Рантайм-поиск библиотеки: `MDBX_SO_PATH` → `mcp/_lib/` → legacy-дефолт →
  системная.

### 9.2 Установка

Установлено отдельно от legacy: `~/.local/share/shared-graph-memory/` (launcher
`server.py`), БД по умолчанию `~/.local/share/shared-graph-memory/db.mdbx`,
имя MCP-инстанса в конфиге opencode — `shared-graph-memory.mdbx`. Старый сервер
(`libmdbx-memory/`) остаётся нетронутым до переключения сессий.

### 9.3 Соглашение об именах (аффиксы к ядру `shared-graph-memory`)

| Контекст | Имя |
|---|---|
| Глобальный (документы/журнал; отличие от системного `mcp_memory`) | `mcp-shared-graph-memory-mdbx` |
| Внутренняя структура и сборка (репо, CI, CMake) | `MCP-shared-graph-memory` / `mcp-shared-graph-memory` |
| Конфиг mcp.json (имя MCP-инстанса) | `shared-graph-memory.mdbx` |
| Каталог установки / рабочая копия | `~/.local/share/shared-graph-memory/` |
| Файлы внутри каталога (без повтора слага) | `server.py`, `db.mdbx` |

В контексте MCP-конфига префикс `mcp-` избыточен; суффикс `-mdbx` подчёркивает
«shared граф на libmdbx» и отличает модуль от системного `mcp_memory`.