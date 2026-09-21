# Test Coverage Map (tests → modules)

> Part of the [Skynet project index](README.md).
> Which tests guard which modules/features — the safety net to consult before refactoring.
> Evidence-based: derived from test names, `#include` usage (public vs white-box), scenario
> semantics and registration in CTest. Registration details: [`build.md`](build.md) §6.5.

---

## 1. Как тесты соотносятся с кодом (факты)

| Способ | Факт |
| --- | --- |
| Публичный API | Подавляющее большинство тестов используют только `mdbx.h` / `mdbx.h++` (с 2026: `#include "mdbx.h++"` в `.c++`; C-тесты — `"mdbx.h"` / `<mdbx.h>`) |
| White-box (включение исходников) | `tests/ut/details_rkl.c` включает `../../src/rkl.c` + `../../src/txl.c` как единицы компиляции; `tests/issues/issue_gh0017.c` включает `../../src/essentials.h` (ABI-layout тест); `tests/framework/base.h++` включает `essentials.h`, `osal.h`, `options.h` + `mdbx.h++` |
| Фреймворк | `mdbx_test` (C++) гоняет сценарии поверх публичного API; внутренние проверки — через `MDBX_CHECKING`/валидацию |

Итог: «серый ящик» — основная стратегия; прямой доступ к внутренностям — точечный
(white-box для структур данных, ABI-тест для раскладки).

## 2. Юнит-тесты `tests/ut/` → модули/фичи

| Тест | Страхуемые модули / фичи |
| --- | --- |
| `details_rkl.c` | **rkl, txl** (white-box: сортированный набор txnid, интервал+список, слияния) |
| `dbi.c++` | **dbi, table**: открытие/переоткрытие хендлов между txn, nested-хендлы, конфликты флагов, дубликаты |
| `txn.c++` | **txn*, mvcc-readers, rthc**: sticky/non-sticky threads, параллельные read+abort, fresh reads, clone тxn (probe_cloning), commit_embark_read |
| `open.c++` | **env.c, dxb, meta**: геометрия (fixed/dynamic), повторные open, много процессов на одном env, live re-open |
| `cursor_closing.c++` | **cursor, dbi**: жизненный цикл курсоров, bind/unbind, закрытие при txn-циклах, многопоточность (case1_thread) |
| `early_close_dbi.c++` | **dbi, env**: закрытие DBI до конца транзакции, повторное использование |
| `dupfix_multiple.c++` | **dpl, node, page-split**: DUPFIXED/DUPSORT множественные вставки, batch read, put_multiple_samelength |
| `dupfix_addodd.c` | **dpl**: нечётные длины данных (DUPFIXED выравнивание) |
| `upsert_alldups.c` | **dml/dpl**: upsert всех дубликатов |
| `crunched_delete.c++` | **cursor, tree-cutoff, page-merge**: массовые удаления, вырожденные случаи, out-of-range prev |
| `bunches_removal.c++` | **tree-cutoff**: bunch-delete, delete_range по типам таблиц |
| `distance_scroll_distribute.c++` | **cursor, tree**: distance/scroll/distribute (в т.ч. deep-уровни), сравнение позиций |
| `doubtless_positioning.c++` | **cursor, tree-search**: все move_operation (lesser/greater/equal, pair/multi), сверка с fullscan |
| `reverse_insertions.c++` | **node, page-split, dpl**: обратные вставки (append/delete направление) |
| `get_cached.c++` | **api-get-cached**: состояния кэша DIRTY/REFRESHED/HIT/CONFIRMED, многопоточность (case2), stairway MVCC |
| `hex_base64_base58.c++` | **utils (транскодеры), slice**: hex/base58/base64 encode/decode |
| `buffers.c++` | **C++ buffer/slice**: reference/inplace/allocated модальности, аллокаторы |
| `maindb_ordinal.c++` | **dbi/table**: ordinal main-db, reopen |
| `nested_drop_abort.c` | **txn-nested, dbi**: drop/abort вложенных |
| `dbi_nested_txn.c` | **txn-nested, dbi**: хендлы в nested txn |
| `rename_dbi.c` | **dbi_rename_locked, defer_free**: переименование таблиц |
| `global_init.c` | **global.c**: глобальная инициализация |
| `probe.c++` | Ограничения/лимиты API (pagesize, размеры) |
| `issue_gh0017.c` | **layout-dxb (ABI)**: `offsetof(tree_t, root) == 8` — защита раскладки on-disk структур |

## 3. Регрессии `tests/issues/` → модули

| Тест | Страхуемое |
| --- | --- |
| `issue_gh0010.c++` | **spill, txn-nested**: утечка `spilled.list` при abort вложенного (dp_limit) |
| `issue_gh0011.c++` | **txn-nested, spill**: рекурсивные nested writer, park/unpark чтения |
| `issue_gh0016.c` | **lck**: lck-less env в одном процессе |
| `issue_gh0023.c++` | **env**: create/remove/recreate, try_insert семантика value_result |
| `issue_gh0024.c++` | **C++ buffer**: move-assign reference → freestanding |
| `issue_gh0025.c++` | **C++ buffer**: is_reference конструктор |
| `issue_gh0026.c++` | **C++ slice/buffer**: строковые/u16string конструкторы и assign |
| `issue_gh0028.c++` | **C++ slice**: safe_middle переполнение |
| `issue_gh0030.c++` | **api-opts**: enable_validation опция при создании env |
| `issue_gh0033.c++` | **cursor::distribute**: пустой вектор |

## 4. Сценарии фреймворка `mdbx_test` → области

| Сценарий | Область покрытия |
| --- | --- |
| `basic` | CRUD-ядро: search/insert/delete/update по всем табличным режимам (см. кейсеты stochastic.sh) |
| `hill` | Многонитевая нагрузка writer/readers, масштабирование читателей |
| `deadread` / `deadwrite` | **rthc, mvcc-readers**: завершение тредов-читателей/писателей, очистка reader-table |
| `forkread` / `forkwrite` | **rthc_afterfork, txn_abort_after_resurrect**: fork-сценарии (POSIX) |
| `jitter` | Отложенные/смещённые операции, тайминги |
| `try` | Попытки/неудачные операции, busy-пути |
| `copy` | **api-copy, walk**: онлайн-копирование (+compactify) |
| `append` | **dml/node**: append-вставки предсортированных данных |
| `ttl` | Метки времени/canary, устаревание |
| `nested` | **txn-nested**: вложенные транзакции |
| `--speculum` | Перекрёстная проверка CRUD с независимой моделью (для малых nops) |

## 5. Матрица «модуль → тесты» (наиболее критичные)

| Модуль/подсистема | Прямое покрытие | Косвенное |
| --- | --- | --- |
| rkl / txl | details_rkl (white-box) | stochastic (GC-пути) |
| cursor / tree-search | cursor_closing, doubts, distance_scroll, crunched | basic/hill |
| node / dpl / page-split | dupfix_*, reverse_insertions, bunches | basic/append |
| tree-cutoff | bunches_removal, crunched_delete | — |
| txn* / mvcc / rthc | txn.c++, issues 10/11, deadread/write, fork* | basic |
| spill | issue_gh0010/0011 | test-* с dp_limit |
| dbi / table | dbi.c++, rename, maindb_ordinal, early_close, nested_drop_abort | basic (multi-map) |
| api-get-cached | get_cached.c++ | — |
| env / dxb / meta | open.c++, issue_gh0023/0030 | smoke_chk, stochastic |
| utils / транскодеры | hex_base64_base58 | — |
| C++ slice/buffer | buffers.c++, issues 24–26/28 | все .c++ тесты |
| GC (gc-get/put) | косвенно (details_rkl на rkl) | stochastic, `MDBX_DEBUG_GCU` |

## 6. Дыры покрытия (кандидаты на расширение быстрых юнит-тестов)

1. **chk-движок** (`mdbx_env_chk`, `chk.c`) — только через `mdbx_chk` в smoke/stochastic;
   нет юнит-тестов API проверки целостности.
2. **defrag** (`mdbx_env_defrag`, `defrag.c`) — только `--copy compactify` /
   `smoke_copy_compactify`; нет прямых тестов defrag-циклов.
3. **osi/lck платформенные ветки** — только кросс-платформенные прогоны CI
   (Linux/Windows/macOS) и `check-posix-locking`; нет юнит-тестов конкретных бэкендов.
4. **meta-переключение/троика, coherency (#269)** — только косвенно через stochastic.
5. **C++ exceptions-семантика** — косвенно (catch mdbx::no_data, db_full в crunched);
   нет отдельного набора «какой код → какое исключение».
6. **audit/refund/bigfoot** — только под `MDBX_CHECKING=2`/спецопциями в stochastic.
7. **Прямые тесты api-*.c тонких обёрток** — большинство через публичный API уже
   покрыто, но «диких» ошибок (неверные аргументы, EINVAL-семантика) — мало.

## 7. Как использовать при рефакторинге

- Перед изменением модуля X: прогнать его прямые тесты (колонка «прямое покрытие») на
  уровнях 1–2, затем полный `ctest` (level 2) и `make smoke` (level 3) — см.
  [`skills/superpowers/verification-before-completion/SKILL.md`](skills/superpowers/verification-before-completion/SKILL.md).
- Для модулей без прямого покрытия (chk, defrag, lck-бэкенды, coherency) — сначала добавить
  быстрый юнит-тест (это одна из целей проекта), затем рефакторить.
- White-box паттерн для структур данных: как `details_rkl.c` — включить `src/<mod>.c`
  напрямую (без public API), подменив `debug_log`.