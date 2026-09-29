# Обзор структуры каталогов (аудит, фаза A, TASK-36)

> Верифицировано на `devel@ec87f4e8` (ветка `feature/task36-structure-review`,
> rebase 2026-09-23 по вердикту REV:c; исходный аудит — `devel@ed5fdd10`).
> Статус исполнения: phase A merged `devel@287c59d2` (origin+upstream);
> C1 (tests/→tests/scripts+docs), C2 (пути регрессий → tests/ut/issues)
> и C3 (.gitignore vs .codeassistant) — исполнены.
> **Сверено с `master@f957a778` (2026-09-29):** документ описывает полную
> структуру старого `devel` (ныне архив `poc1-failed`). После переноса
> доработок в новую ветку `devel` и `master` часть слоя роя осталась вне
> master: `.codeassistant/` и `.codeassistantmodes` отсутствуют на master/devel
> (только в `poc1-failed`); счётчики файлов на master: `tests/` = 79,
> `skynet/` = 57, `src/` = 119 (совпадает). Выводы F1–F12 и решения C1–C9
> (кроме численных счётчиков) сохраняют силу.
> Документ — результат **фазы A** аудита: карта текущей структуры,
> находки (включая «дубли» `chk.c`/`defrag.c`), кандидаты улучшений с оценкой
> риска/ценности и решение «трогаем / не трогаем». Фазы B (план переносов),
> C (исполнение), D (верификация) выполняются отдельными батчами после
> согласования с ревьюверами и в окне без параллельных задач.
>
> Ограничения, действующие на все решения ниже (TASK-36.md):
> 1. `make dist` (амальгама) должен оставаться зелёным; пути релизного набора —
>    синхронно в amalgam-механике (`dist-cutoff`-маркеры, `DIST_SRC`/`DIST_EXTRA`,
>    `skynet/build.md §7`).
> 2. Upstream-паритет: канонический для `dqdkfa/libmdbx` набор (`src/` ядро,
>    корневые `mdbx.h`/`mdbx.h++`, `docs/Doxyfile.in`, man-страницы) **не
>    реструктурируется** без явного одобрения владельца. Фокус — нашим слоям:
>    `tests/`, `skynet/`, `.codeassistant/`, доки, мусор/дубли.
> 3. No merge-conflict generation: финальные переносы — единый батч в окне
>    без параллельных задач.
> 4. Все ссылки на изменяемые пути обновляются в том же коммите.

---

## 1. Карта текущей структуры (по `git ls-files`, 2026-09-23)

### 1.1 Корень репозитория

| Путь | Что это | Принадлежность |
| --- | --- | --- |
| `mdbx.h`, `mdbx.h++`, `mdbx++/` (17 `h++`) | Публичный C/C++ API | upstream (канон) |
| `src/` (119 файлов) | Ядро движка + инструменты + man | upstream (канон) |
| `CMakeLists.txt`, `GNUmakefile`, `Makefile`, `conanfile.py`, `cmake/` | Сборка | upstream |
| `ChangeLog.md`, `ChangeLog-0.09…0.14.md`, `ChangeLog-Old.md` (8 шт.) | Ченджлоги версий | upstream (канон, на `master`) |
| `COPYRIGHT`, `LICENSE`, `NOTICE`, `TODO.md`, `README.md`, `AGENTS.md` | Правовые/README | upstream (AGENTS.md — на `master`) |
| `.le.ini`, `.clang-format`, `.cmake-format.yaml`, `valgrind.supp`, `.gitignore` | Дотфайлы | upstream (`.le.ini` есть на `master`) |
| `docs/` (13 файлов) | Doxygen: `Doxyfile.in`, `_*.md`, CSS/HTML, `ld+json`, `title`, `sitemap.add` | upstream (канон) |
| `examples/` (5 + `pcrf/` 2) | Примеры + PCRF-симулятор | upstream |
| `.github/workflows/` (7 yml), `.sourcecraft/ci.yaml` | CI | upstream |
| `tests/` (112 файлов) | Тесты (framework/ut/issues/exploits/scripts/docs) | **наш слой** |
| `skynet/` (67 файлов) | Рой-доки и SKILLS | **наш слой** |
| `.codeassistant/` (44 файла) + `.codeassistantmodes` (итого 45) | Конфиг/скиллы CodeAssist | **наш слой** |

Devel-only top-level (отличия от `master`): только `.codeassistant/`,
`.codeassistantmodes`, `skynet/`. Всё остальное на верхнем уровне идентично
upstream `master`.

### 1.2 `src/` — ядро

- **API-реализация `api-*.c` — 14 файлов**: api-cold, api-copy, api-cursor,
  api-dbi, api-env, api-extra, api-gc, api-get-cached, api-key-transform,
  api-misc, api-opts, api-range-estimate, api-txn, api-txn-data.
  Декомпозиция по doxygen-группам `mdbx.h` (коммит `ba6df2bb`, 2024-12-17,
  присутствует и на `master`).
- **Движок**: txn-семейство (txn, txn-basal, txn-nested, txn-ro, txl),
  дерево/курсоры (node, dbi, dpl, dml, cursor, page-*, tree-*, walk, sort,
  comparators), storage (meta, table, dxb, histogram, pnl, global),
  GC (gc-get, gc-put, rkl, refund, spill, coherency), OS (osal, lck*, atomics*,
  unaligned, windows-*), сервис (utils, audit, logging_and_debug, cogs,
  **chk, defrag**), mvcc-readers, rthc, dml/dpl.
- **Инструменты**: `src/tools/{chk,copy,stat,dump,load,drop,defrag}.c` +
  `wingetopt.{c,h}` (Windows). В non-amalgamated сборке из них собираются
  `mdbx_*` (CMakeLists.txt:1355), в амальгаме — из корневых `mdbx_*.c`
  (генерируются `dist-extra-rule`).
- **Man-страницы**: `src/man1/mdbx_{chk,copy,drop,dump,load,stat}.1`
  (единственный источник; `MAN_SRCDIR := src/man1/`, GNUmakefile:566;
  дистрибутивные `man1/` создаются в `dist`).
- **Прочее**: `amalgam.in` (шапка амальгамы), `bits.md` (битовые маски,
  в т.ч. в IDE-target `non-code` CMakeLists.txt:1069), `config.h.in`,
  `version.c.in`, `ntdll.def`, safeseh-ассемблеры/объект.

### 1.3 `tests/` — тесты

| Подкаталог / файл | Назначение |
| --- | --- |
| `tests/framework/` (32 ф.) | Каркас `mdbx_test` (base, config, keygen, log, chrono, fork, nested, copy, dead, hill, jitter, ttl, try, main, cases, append, test, osal-*, stub/) |
| `tests/ut/api|cursor|cxx|dbi|env|gc|txn/` | Модульные тесты по областям (62 ф., GoogleTest) |
| `tests/ut/issues/` | Регрессии по issues: `issue_gh0010…gh0033` (+ собственный `CMakeLists.txt`) |
| `tests/exploits/` | PoC-и: `poc-node_ds-oob.c`, `pos-badgeo-oos.c` |
| `tests/ci/` | CI: `ci.sh` (вход; используется `.github/workflows/*.yml` и `.sourcecraft/ci.yaml` как `test*/ci/ci.sh`), `config.json` + `run-cell.sh` (TASK-28 testing-infra-v2) |
| `tests/scripts/` | Скрипты + отладка (после C1): `stochastic.sh`, `select-tests.sh`, `battery-tmux.sh`, `probes-check.sh`, `dump-load.sh`, `probe-mdbx-multiple-iovlen.c`, `tmux.conf`, `.gdbinit`, `with.gdb` |
| `tests/docs/` | Документация (после C1): `README.md`, `mdbx-test-options.md`, `LICENCE` |
| `tests/CMakeLists.txt` | Регистрация тестов |

### 1.4 Прочее

- `skynet/` — индекс проекта, архитектура, протокол, SKILLS, codex-и.
- `.codeassistant/` — MCP-конфиг + скиллы (refactor, modern-cpp-en,
  libmdbx-amalgamated) + rules-skill-writer.
- `dist/`, `@dist-check*`, `cmake-build-*` — артефакты сборки/амальгамы
  (в .gitignore, в git не трекаются).

---

## 2. Находки

### F1. `src/chk.c`/`src/defrag.c` против `src/tools/chk.c`/`src/tools/defrag.c` — НЕ дубликаты (тёзки)

Проверено против кода и сборки — гипотеза «остатки неполной миграции в
`src/tools/`» **не подтвердилась**:

| Файл | Роль | Где используется |
| --- | --- | --- |
| `src/chk.c` (1806 стр.) | Библиотечная реализация проверки БД | `int mdbx_env_chk(...)` определён здесь (src/chk.c:1724); собран в `libmdbx` (LIBMDBX_SOURCES, CMakeLists.txt:983) |
| `src/defrag.c` (1257 стр.) | Библиотечная реализация дефрагментации | `defrag_result(dfc_t*, ...)` (src/defrag.c:8), объявлен в `src/gc.h:172` как `MDBX_INTERNAL`; вызывается из `mdbx_env_defrag()` в `src/api-gc.c:99,203`; собран в `libmdbx` (CMakeLists.txt:993) |
| `src/tools/chk.c` (663 стр.) | CLI `mdbx_chk` на публичном API | `main()` вызывает `mdbx_env_chk()` (src/tools/chk.c:619); собирается как исполняемый `mdbx_chk` (CMakeLists.txt:1355) |
| `src/tools/defrag.c` (429 стр.) | CLI `mdbx_defrag` на публичном API | `main()` вызывает `mdbx_env_defrag()` (src/tools/defrag.c:390) |

Свидетельства:
- Обе пары существовали на `origin/master` до нашего форка-зеркала; введены
  коммитом `3de3d425` «изменение лицензии и реструктуризация исходного кода».
- Артефакты сборки подтверждают разделение TU: `mdbx.dir/src` (библиотека)
  и `mdbx_chk.dir/src/tools`, `mdbx_defrag.dir/src/tools` (исполняемые).
- Имена файлов совпадают только по базовому имени — это **naming collision**,
  а не дублирование кода. Мёртвого кода нет: оба корневых файла — часть
  библиотеки и необходимы (иначе сборка сломалась бы на undefined symbols).

**Решение: НЕ трогаем** (upstream-канон, constraint #2). Опционально —
просьба к владельцу о переименовании корневых `chk.c`→`engine-chk.c` /
`defrag.c`→`engine-defrag.c` (или перенос CLI-части в `tools/` уже сделан —
он на месте), но это требует отдельного ADR и синхронных правок
CMakeLists/GNUmakefile/alphabetical-order. Ценность — устранение путаницы при
навигации; риск — боль на каждом слиянии с upstream. Откладываем.

### F2. Гранулярность `src/api-*.c` — осознанная, upstream-канон

14 файлов — декомпозиция по функциональным группам `mdbx.h` (введена
`ba6df2bb`, 2024-12-17, есть на `master`). Позволяет параллельную компиляцию
TU в non-alloy-режиме и читаемую историю правок per-family. Упоминание
«20+ файлов» в ТЗ задачи завышено — фактически 14.

**Решение: НЕ трогаем.**

### F3. `tests/` — корень смешивал скрипты, отладку и доки (исправлено в C1)

Исходно в корне `tests/` лежали три разнородные группы:

1. **Скрипты**: `stochastic.sh`, `select-tests.sh`, `battery-tmux.sh`,
   `probes-check.sh`, `dump-load.sh`, `probe-mdbx-multiple-iovlen.c`;
2. **Отладочные конфиги**: `.gdbinit`, `with.gdb`, `tmux.conf`;
3. **Доки/прочее**: `README.md`, `mdbx-test-options.md`, `LICENCE`.

**Исполнено (C1)**: группы 1-2 → `tests/scripts/`, группа 3 → `tests/docs/`.
Внутренние относительные ссылки сохранены: `stochastic.sh` использует
`${SCRIPT_DIR}/.gdbinit` + `${SCRIPT_DIR}/with.gdb`, `battery-tmux.sh` —
`${DIR}/stochastic.sh` + `${DIR}/tmux.conf`; файлы перенесены вместе, скрипты
работают без правок тела.

Точки ссылок, обновлённые в том же коммите C1:
- `GNUmakefile:623` `tests/scripts/select-tests.sh`;
- `tests/CMakeLists.txt:454,463,473` `${CMAKE_CURRENT_SOURCE_DIR}/scripts/stochastic.sh`;
- `.sourcecraft/ci.yaml` — `test*/ci/ci.sh` (не затронут: `tests/ci/` на месте);
- `.github/workflows/*.yml` (7 файлов) — `tests/ci/ci.sh` (не затронут);
- корневой `README.md`, `AGENTS.md`, `skynet/*.md` + SKILLS (~30 ссылок).

**Решение: ТРОГАЕМ, исполнено батчем C1.** `tests/ci/`, `tests/ut/`,
`tests/framework/` остались на месте.

### F4. Документ-дрейф: в доках был указан несуществующий путь (исправлено в C2)

Регрессии физически лежат в **`tests/ut/issues/`** (`add_subdirectory(ut/issues)`,
tests/CMakeLists.txt:609; `tests/ut/issues/CMakeLists.txt`), но **16 файлов**
ссылались на несуществующий путь без префикса `ut/`: 9 доков
(`skynet/README.md`, `build.md`, `cxx-api.md`, `structure.md`,
`test-coverage.md`, `skynet-protocol.md`, `build-deps-codex.md`,
`test-scenarios.md`, `workflows.md`), 6 SKILLS superpowers
(`README`, `brainstorming`, `requesting-code-review`,
`systematic-debugging`, `test-driven-development`, `writing-plans`)
и кодовая мёртвая ветка в `tests/scripts/select-tests.sh` (удалена в phase A).

**Исполнено (C2)**: пути в 15 текстовых файлах приведены к
`tests/ut/issues/` в одном батче. Мёртвая ветка `select-tests.sh` была
удалена ранее (merge phase A). Риск был ~нулевой (правки текстов), ценность
для онбординга средняя.

### F5. ChangeLog-*.md в корне — upstream-конвенция

8 файлов на корне идентичны `master`. `GNUmakefile` (DIST_EXTRA=…`ChangeLog.md`;
doxygen-target строит `docs/overall.md|intro.md|usage.md` через
`md-extract-section` из `ChangeLog.md`) завязан на корневое размещение.
Перенос в подкаталог сломает сборку доков и upstream-слияния.

**Решение: НЕ трогаем.** Кандидат на группировку отклонён (риск высокий,
ценность низкая).

### F6. `skynet/` внутри публичного репо

Осознанно: рою нужен общий git-канал для доков/SKILLS; из амальгамы исключён
(не входит в `DIST_SRC`/`DIST_EXTRA`, build.md §7.3). Альтернатива (вынос за
пределы репо) — вопрос workspace-инфраструктуры, вне scope задачи.

**Решение: НЕ трогаем.**

### F7. `.codeassistant/` трекался вопреки `.gitignore` (исправлено в C3)

`.gitignore` ранее игнорировал `.codeassistant*`, но 44 файла (`.codeassistant/`)
+ `.codeassistantmodes` уже отслеживались (добавлены ранее; tracked ≠ ignored).
Каталог содержит MCP-конфиг, правила
`rules-skill-writer` и скиллы; на них ссылается `skynet/README.md §4` — рою
как справочники нужны. Это было противоречие (gitignore обещал, что каталога
в репо нет, а он есть).

**Исполнено (C3)**: широкий паттерн `.codeassistant*` заменён на точечные
`.codeassistant/tmp/` + `.codeassistant/cache/` с комментарием «tracked by
design». Риск ~нулевой, дрейф устранён.

### F8. `docs/` — upstream-канон, реструктуризация запрещена

`docs/Doxyfile.in`, `_*.md`, CSS/HTML, `ld+json` идентичны `master`
(проверено `git ls-tree origin/master`). «S.scribe/» из ТЗ задачи фактически
является каталогом `docs/`. Doxygen-страницы генерируются
(`docs/overall.md|intro.md|usage.md` не в git; `docs/html/`, `docs/Doxyfile`
в .gitignore).

**Решение: НЕ трогаем.**

### F9. `examples/pcrf/` — самодостаточный подпроект, ок

`pcrf/pcrf_simulator.c` + `pcrf/README.md`; собирается отдельным таргетом
`pcrf_simulator` (examples/CMakeLists.txt); входит в `DIST_EXTRA`. Структура
адекватна.

**Решение: НЕ трогаем.**

### F10. Man-страницы — единственный источник в `src/man1/`

`MAN_SRCDIR` переключается (GNUmakefile:478 amalgamated `man1/` vs :566
non-amalgamated `src/man1/`); дистрибутивные `man1/` в `dist` генерируются
`dist-extra-rule` (GNUmakefile:979). Дубликатов в dev-репо нет.

**Решение: НЕ трогаем.**

### F11. `src/bits.md` — на каноне, трогать нельзя

Одиночный `.md` в `src/` (битовые маски), присутствует на `master` и входит
в IDE-target `non-code` (CMakeLists.txt:1069).

**Решение: НЕ трогаем.**

### F12. `tests/ut/` — внутренняя организация уже хорошая

Тесты разложены по областям (`api|cursor|cxx|dbi|env|gc|txn`), issues —
вложенный подкаталог с собственным CMakeLists, метки `ut.*` на месте
(основа `tests/scripts/select-tests.sh` и документа `skynet/test-coverage.md`).
Единственный дрейф — устаревший список файлов в `skynet/structure.md:199`
(покрывается F4).

**Решение: НЕ трогаем** (помимо правки доков в F4).

---

## 3. Кандидаты улучшений — риск / ценность / решение

| № | Кандидат | Ценность (для разработки) | Риск (merge/amalgam/upstream) | Решение |
| --- | --- | --- | --- | --- |
| C1 | `tests/` → `tests/scripts/` + `tests/docs/` (группировка корня) | средняя (находимость, чистота) | средний (~46 несамоссылочных ссылок в 22 файлах) | **Исполнено (C1)** |
| C2 | Пути регрессионных тестов приведены к `tests/ut/issues/` (15 файлов: 9 доков + 6 SKILLS; мёртвая ветка `select-tests.sh` удалена в phase A) | средняя (онбординг, снижение ложных поисков) | нулевой (тексты) | **Исполнено (C2)** |
| C3 | `.gitignore` vs `.codeassistant/` (противоречие) | низкая (гигиена) | нулевой | **Исполнено (C3)** |
| C4 | Переименование тёзок `src/chk.c`/`src/defrag.c` | низкая (устранение путаницы) | высокая (upstream-паритет, constraint #2; правки 4+ build-файлов, история) | **НЕ трогаем**; опция — ADR владельцу |
| C5 | `src/api-*.c` гранулярность | — | — | **НЕ трогаем** (канон) |
| C6 | ChangeLog-*.md группировка | низкая | высокая (upstream, doxygen-конвейер) | **НЕ трогаем** |
| C7 | `skynet/` вынос из публичного репо | — | — | **НЕ трогаем** (вне scope; вопрос инфраструктуры) |
| C8 | `docs/` реструктуризация | — | высокая (upstream-канон) | **НЕ трогаем** |
| C9 | Man-страницы/`examples/`/`src/bits.md` | — | — | **НЕ трогаем** (канон, структура адекватна) |

---

## 4. Рекомендации для фаз B/C/D (статус исполнения)

1. **Фаза B** — карта переносов только для C1–C3 (ниже). Согласована
   координатором (GO TASK-36, mail-GO36).
2. **Порядок батчей** (окно без параллельных задач):
   - **C1 — ИСПОЛНЕНО**: `git mv` скриптов+конфигов в `tests/scripts/`,
     доков в `tests/docs/`; все ссылки обновлены в том же коммите
     (README.md, AGENTS.md, GNUmakefile, tests/CMakeLists.txt, skynet/*, SKILLS).
   - **C2 — ИСПОЛНЕНО**: пути регрессионных тестов в доках приведены к
     фактическому каталогу `tests/ut/issues/` в 15 текстовых файлах
     (9 доков + 6 SKILLS, перечень §F4). Мёртвая ветка `select-tests.sh`
     удалена ранее в phase A.
   - C3 — правки `.gitignore` — без `git mv`.
3. **Верификация после каждого батча (фаза D)**: Linux (gcc+clang) сборка +
   P2 `ctest -L 'ut\.' -LE 'ut\.heavy'`; затем `make dist` (зелёная амальгама);
   `grep` по старым путям = 0. Windows-риски гасит GitHub CI (по согласованию
   с владельцем, квота).
4. **Негативные решения** (C4–C9) зафиксированы выше — они не требуют
   действий, но должны быть видны ревьюверам, чтобы не предлагать их повторно.

---

## 5. Источники и метод

- Карта построена по `git ls-files` и `git ls-tree origin/master`
  (сверка с upstream-каноном), сверена с `skynet/structure.md` (актуален
  по содержанию; дрейф путей регрессий устранён батчем C2).
- Факты сборки подтверждены чтением `CMakeLists.txt` (строки 983, 993, 1069,
  1333-1363, 185-193, 67-73) и `GNUmakefile` (478, 566, 623, 540-545, 959,
  979); артефакты `cmake-build-t41-check` (mdbx.dir/src, mdbx_chk.dir/src/tools).
- После REV:c (D.reviewer-c, needs-work minor) исправлены: счётчики файлов
  (`.codeassistant*` = 45, `mdbx++/` = 17), инвентарь C2 расширен до 16 файлов
  (+ мёртвая ветка в `tests/scripts/select-tests.sh:104`), ветка rebase на актуальный
  `devel@ec87f4e8`, факты перепроверены на новой базе.
- Итог: **дубликатов-кандидатов на удаление не найдено**; реальных кандидатов
  на перенос — 3 (C1–C3), все в нашем слое, upstream-набор не затрагивается.