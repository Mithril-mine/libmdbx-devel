# White spots & test-gap plan (test-coverage-gaps)

> Рабочий документ: актуальные «белые пятна» покрытия (data: 2026-10-29,
> merged-map linux+win32+macos, 6612 symbols / 37078 blocks) и сценарии для
> их закрытия. Формат сценариев — по `test-scenarios-codex.md`; полные SC
> нумерованные — в `test-scenarios.md`.

## 1. Как измерено

- Per-test gcov-покрытие (`run_coverage.py`, 212 тестов, Linux) загружено
  в shared-graph-memory DB: `coverage_summary` по модулям.
- Тоталь (Linux): **75.9%** (28281/37284 blocks), символы 3496/6612.
- Windows-модули (`windows-import.c`, `lck-windows.c`) исполняются только в
  windows-билдах; клетка `win-mingw-64-debug-coverage` в CI (coverage-профиль)
  добавляет их gcov — после стабилизации артефакта проценты обновятся.
  До этого — они показаны «0%» (не измеряются в Linux-cпрогоне).

## 2. Актуальные белые пятна (ранжирование: крупные → малые)

> `uncovered_blocks = blocks_total - blocks_covered`.

### P1 — крупные, ядро

| Модуль | blocks uncovered | pct | Гипотеза причины |
| --- | --- | --- | --- |
| `tree-search` | 1527 | 40.1% | Ветви B+tree-поиска: переходы страниц, dup-границы, направление — частично недостижимы через API, требуют спец-состояний дерева |
| `mdbx` (c++) | 588 | 58.7% | C++ API: часть перегрузок/error-кодов не покрыта отдельно |
| `decl_buffer` | 498 | 77.7% | C++ buffer/slice: редкие ветви (view→buffer, oversized) |
| `osal` | 376 | 61.2% | Платформенные osal-пути (память, файлы) в альтернативных конфигурациях |
| `cursor` | 365 | 87.0% | Нестандартные позиционирования курсора |

### P2 — значимые

| Модуль | blocks uncovered | pct | Примечание |
| --- | --- | --- | --- |
| `gc-put` | 361 | 67.1% | GC-цепочки при крупных освобождениях (SC-2 частично) |
| `lck-posix` | 339 | 60.9% | Lock-файлы: конкуренция, старые замки, EAGAIN-пути |
| `gc-get` | 284 | 68.2% | GC-поиск, pipelining |
| `defrag` | 262 | 66.7% | Defrag-циклы, LAGGARD_READER (SC-12 частично) |
| `decl_transcoders` | 179 | **11.4%** | C++ транскодеры: только часть типов покрыта |
| `impl_env` | 160 | 50.8% | C++ env_managed/geо geometry |
| `chk` | 156 | 79.3% | chk-движок (SC-11 частично) |

> `audit` (`src/audit.c`) — вспомогательный отладочный аудит, работает только
> под `MDBX_CHECKING`; **покрытие тестами для него не требуется**.

### P3 — малые (быстрые выигрыши)

| Модуль | blocks uncovered | pct | Примечание |
| --- | --- | --- | --- |
| `decl_core` | 73 | 20.7% | C++ core-исключения→коды |
| `decl_txn` | 62 | 31.9% | C++ txn-обёртки |
| `internals` | 39 | 2.5% | Внутр. инициализация |
| `essentials` | 6 | 50.0% | Атомик-ветви |
| `end` | 55 | 5.2% | Завершение/ресурсы |
| `decl_env` | 100 | 25.4% | C++ env API |

> **Приоритет по эффекту**: P1+P2 дают ≈ 4/5 непокрытых блоков; P3 —
> дёшево и быстро (повышает чистоту summary).

> **cursor (validate-функции, 354 блока) — N/A (отложено)**: `cursor_validate`,
> `cursor_validate_updating`, `unexpected_dupsort` — отладочные проверки под
> MDBX_CHECKING/AUDIT (решение владельца: задокументировать и отложить, как
> audit). Покрытие возможно только CHECKING-клеткой.

> **Windows-модули (windows-import/lck-windows)**: замер gcov на
> windows-latest/mingw-gcc-15.2 не работает (4 попытки: -lgcov,
> --no-gc-sections, driver --coverage, GCOV_PREFIX — .gcda не пишутся при
> валидных .gcno и успешном __gcov_dump; см. БЗ
> bug:ci:win-mingw-gcda-exhausted). Инфраструктура клетки и шага остаётся;
> вернуться при смене toolchain в клетке. Не включать их проценты в
> приоритизацию.

## 3. Сценарии для закрытия (краткие; полные SC-61+ — в test-scenarios.md)

### SC-67. lck-posix contention — ✅ реализован (#278)

### SC-69. tree-search comparators — ✅ реализован (#273)

### SC-61. tree-search: ветви перехода страницы (unit) — ✅ реализован (#242)

- **hypothesis**: `mdbx_get`/курсор на ключах, разнесённых по страницам,
  покрывает все точки сравнения в `search-inpage`/`search-paged` (tab-переходы).
- **steps**: вставить N ключей (переполнение 2+ страниц), для каждого искать
  exact / нижний / верхний (prev, first, last); проверить call-count по
  `tree-search` через coverage.
- **expectations**: блоки `node_search`/`search_in_page` и ветви сравнения ≥95% покрыты.
- **speed_class**: unit; **sync_mode**: nosync-safe; **probe_points**:
  `search_cmp`, `node_search`, `page_get_by_index`.

### SC-62. tree-search: dup-границы (unit) — ✅ реализован (#242)

- **hypothesis**: DUP-ключи (MDBX_DUPSORT) неправильно расходятся с пограничными
  поисками.
- **steps**: DUP-таблица с 3+ значениями на ключ; поиск first/last/prev/next,
  равенство полное/частичное.
- **expectations**: ветви dup-сравнения покрыты; результаты корректны.

### SC-63. decl_transcoders: полное покрытие типов C++ (unit) — ✅ реализован (#245)

- **hypothesis**: каждый C++ тип данных прогоняется через транскодер в
  `mdbx::buffer`/`slice`.
- **steps**: прогон всех целочисленных/плавающих/строковых типов + комбositions
  (buffer из slice, slice из buffer, парсинг hex).
- **expectations**: `decl_transcoders` pct≥80.

### SC-64. decl_core: исключения ↔ коды C API (unit) — ✅ реализован (#245)

- **hypothesis**: каждый `MDBX_*` код маппится на своё C++ исключение
  (mdbx::exception и подклассы).
- **steps**: серия вызовов, возвращающих каждый ошибку (ключ не найден,
  map full, invalid, readonly...) и проверка пойманного исключения.
- **expectations**: покрыты все маппинги; сообщения exact.

### SC-70. defrag engine — ✅ реализован (#274)

### SC-65. gc-get/gc-put: цепочки ретирирования (unit/smoke) — ✅ реализован (#257)

### SC-71. gc-dense packing — ✅ реализован (#275)

- **hypothesis**: массовое удаление/обновление формирует корректные GC-цепочки
  (retire/reclaim) и pipelining — ветви `gc-get`/`gc-put` покрываются.
- **steps**: наполнить БД; удалить большой диапазон (bunch-delete); выполнить
  commit — последовательно 2-3 раза; прогнать `mdbx_chk`.
- **expectations**: `gc-get`/`gc-put` pct≥85; страницы корректно возвращены в GC.

### SC-66. osal: платформенные ветви (smoke, кроссплатформенно)

- **hypothesis**: альтернативные osal-пути (mincore-нет, MAP_POPULATE-нет,
  seek-ветви) исполняются в подходящей конфигурации.
- **steps**: сборка с `MDBX_USE_MINCORE=OFF`, прогон smoke; на Windows-win
  части — через покрытие win-клетки.
- **expectations**: pct osal растёт, без регрессий.

### SC-67. многопроцессная конкуренция lck-posix (smoke-heavy)

- **hypothesis**: lock-файл корректно конкурирует между процессами (включая
  EAGAIN-пути).
- **steps**: 2+ процесса открывают одну БД, чередуют записи.
- **expectations**: `lck-posix` EAGAIN-ветви покрыты, нет deadlock.

### SC-68. C++ env/txn/cursor: покрыть редкие ветви (unit-scratch) — ✅ реализован (#258)

- **hypothesis**: nullable-обёртки, moved-from объекты, исключения из
  деструкторов — корректны.
- **steps**: необычный жизненный цикл (move, RVO, abort-Tree).
- **expectations**: `impl_env`/`decl_env`/`decl_txn`/`decl_cursor` pct растут.

## 4. Порядок исполнения

1. ~~**SC-61/62 (tree-search)** и **SC-65 (gc-get/gc-put)**~~ — ✅ реализованы (#242, #257).
2. ~~**SC-63/64 (decl_transcoders, decl_core)**~~ — ✅ реализованы (#245).
3. **SC-66/67** (osal, lck) — платформенные.
4. ~~**SC-68**~~ — ✅ реализован (#258).
5. После каждого — локальный gcov-прогон теста → `coverage_blocks` растёт.

## 5. Ограничения и как таблица обновится

- После стабилизации win-gcov артефакта — `windows-import`/`lck-windows`
  перестанут показывать «0%» (см. §1); таблица пересчитывается из DB.
- Тоталь меняется по мере добавления тестов — следить не за %
  безусловно, а за покрытием **целевых модулей** из таблицы.