# Обзор структуры каталогов (аудит, фаза A, TASK-36)

> Верифицировано на `devel@ed5fdd10` (ветка `feature/task36-structure-review`,
> 2026-09-23). Документ — результат **фазы A** аудита: карта текущей структуры,
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
| `mdbx.h`, `mdbx.h++`, `mdbx++/` (19 `h++`) | Публичный C/C++ API | upstream (канон) |
| `src/` (119 файлов) | Ядро движка + инструменты + man | upstream (канон) |
| `CMakeLists.txt`, `GNUmakefile`, `Makefile`, `conanfile.py`, `cmake/` | Сборка | upstream |
| `ChangeLog.md`, `ChangeLog-0.09…0.14.md`, `ChangeLog-Old.md` (8 шт.) | Ченджлоги версий | upstream (канон, на `master`) |
| `COPYRIGHT`, `LICENSE`, `NOTICE`, `TODO.md`, `README.md`, `AGENTS.md` | Правовые/README | upstream (AGENTS.md — на `master`) |
| `.le.ini`, `.clang-format`, `.cmake-format.yaml`, `valgrind.supp`, `.gitignore` | Дотфайлы | upstream (`.le.ini` есть на `master`) |
| `docs/` (13 файлов) | Doxygen: `Doxyfile.in`, `_*.md`, CSS/HTML, `ld+json`, `title`, `sitemap.add` | upstream (канон) |
| `examples/` (5 + `pcrf/` 2) | Примеры + PCRF-симулятор | upstream |
| `.github/workflows/` (7 yml), `.sourcecraft/ci.yaml` | CI | upstream |
| `tests/` (110 файлов) | Тесты (framework/ut/issues/exploits/scripts/docs) | **наш слой** |
| `skynet/` (61 файл) | Рой-доки и SKILLS | **наш слой** |
| `.codeassistant/` (33 файла) + `.codeassistantmodes` | Конфиг/скиллы CodeAssist | **наш слой** |

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
  (единственный источник; `MAN_SRCDIR := src/man1/`, GNUmakefile:562;
  дистрибутивные `man1/` создаются в `dist`).
- **Прочее**: `amalgam.in` (шапка амальгамы), `bits.md` (битовые маски,
  в т.ч. в IDE-target `non-code` CMakeLists.txt:1069), `config.h.in`,
  `version.c.in`, `ntdll.def`, safeseh-ассемблеры/объект.

### 1.3 `tests/` — тесты

| Подкаталог / файл | Назначение |
| --- | --- |
| `tests/framework/` (30 ф.) | Каркас `mdbx_test` (base, config, keygen, log, chrono, fork, nested, copy, dead, hill, jitter, ttl, try, main, cases, append, test, osal-*, stub/) |
| `tests/ut/api|cursor|cxx|dbi|env|gc|txn/` | Модульные тесты по областям (56 ф., GoogleTest) |
| `tests/ut/issues/` | Регрессии по issues: `issue_gh0010…gh0033` (+ собственный `CMakeLists.txt`) |
| `tests/exploits/` | PoC-и: `poc-node_ds-oob.c`, `pos-badgeo-oos.c` |
| `tests/ci/ci.sh` | CI-вход (используется `.github/workflows/*.yml` и `.sourcecraft/ci.yaml` как `test*/ci/ci.sh`) |
| `tests/*.sh` (корень) | `stochastic.sh`, `select-tests.sh`, `battery-tmux.sh`, `probes-check.sh`, `dump-load.sh` |
| `tests/*` (корень, прочее) | `probe-mdbx-multiple-iovlen.c`, `tmux.conf`, `.gdbinit`, `with.gdb`, `README.md`, `mdbx-test-options.md`, `LICENCE`, `CMakeLists.txt` |

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

### F3. `tests/` — корень смешивает скрипты, отладку и доки

Внутри `tests/` уже есть чистая разбивка (`ut/` по областям, `framework/`,
`ci/`), но в корне лежат три разнородные группы:

1. **Скрипты**: `stochastic.sh`, `select-tests.sh`, `battery-tmux.sh`,
   `probes-check.sh`, `dump-load.sh`, `probe-mdbx-multiple-iovlen.c`;
2. **Отладочные конфиги**: `.gdbinit`, `with.gdb`, `tmux.conf`;
3. **Доки/прочее**: `README.md`, `mdbx-test-options.md`, `LICENCE`.

Кандидат: `tests/scripts/` (скрипты+конфиги), `tests/docs/`
(README.md, mdbx-test-options.md, LICENCE) — либо минимально: скрипты в
`tests/scripts/`, остальное оставить.

Точки ссылок (полный список для фазы B, проверен):
- `GNUmakefile:619` `tests/select-tests.sh`; `GNUmakefile:636` build-stochastic
  (см. `ctest-scenario-run`, `@cmake-stochastic-build`);
- `tests/CMakeLists.txt:416,425,435` `${CMAKE_CURRENT_SOURCE_DIR}/stochastic.sh`;
- `.sourcecraft/ci.yaml` — `test*/ci/ci.sh` (wildcard! перенос `tests/ci/`
  потребует правки yaml);
- `.github/workflows/*.yml` (7 файлов) — `tests/ci/ci.sh`;
- `skynet/*.md` + SKILLS — ~30 ссылок на `tests/stochastic.sh`,
  `tests/battery-tmux.sh`, `tests/select-tests.sh`, `tests/probes-check.sh`,
  `tests/ci/ci.sh`.

**Решение: ТРОГАЕМ (фаза C, один батч)** — умеренная ценность (находимость,
чистота корня тестов), средний риск из-за числа ссылок; все ссылки обновляются
в том же коммите (constraint #4). `tests/ci/` и `tests/ut/`, `tests/framework/`
остаются на месте.

### F4. Документ-дрейф: в доках указан несуществующий путь `tests/issues/`

Регрессии физически лежат в **`tests/ut/issues/`** (`add_subdirectory(ut/issues)`,
tests/CMakeLists.txt:571; `tests/ut/issues/CMakeLists.txt`), но 8+ документов
упоминают `tests/issues/`:
`skynet/README.md:42`, `skynet/build.md:157,257`, `skynet/cxx-api.md:99,106`,
`skynet/structure.md:200` (+ устаревший список `tests/ut` в :199),
`skynet/test-coverage.md:15,50`, `skynet/skynet-protocol.md:331`,
`skynet/build-deps-codex.md:70`.

**Решение: ТРОГАЕМ (фаза C, тривиально)** — правим упоминания путей в доках;
риск ~нулевой, ценность для онбординга средняя. Это не перенос кода.

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

### F7. `.codeassistant/` трекается вопреки `.gitignore`

`.gitignore:77` игнорирует `.codeassistant*`, но 33 файла уже отслеживаются
(добавлены ранее; tracked ≠ ignored). Каталог содержит MCP-конфиг, правила
`rules-skill-writer` и скиллы; на них ссылается `skynet/README.md §4` — рою
как справочники нужны. Это противоречие (gitignore обещает, что каталога
в репо нет, а он есть).

**Решение: ТРОГАЕМ (фаза C, микро-батч)** — уточнить `.gitignore`
(заменить строку `.codeassistant*` на точечные паттерны для временного мусора,
напр. `.codeassistant/tmp/` и пр.), либо явно задокументировать
«tracked by design». Ценность низкая, риск ~нулевой, устраняет дрейф.

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

`MAN_SRCDIR` переключается (GNUmakefile:474 amalgamated `man1/` vs :562
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
(основа `tests/select-tests.sh` и документа `skynet/test-coverage.md`).
Единственный дрейф — устаревший список файлов в `skynet/structure.md:199`
(покрывается F4).

**Решение: НЕ трогаем** (помимо правки доков в F4).

---

## 3. Кандидаты улучшений — риск / ценность / решение

| № | Кандидат | Ценность (для разработки) | Риск (merge/amalgam/upstream) | Решение |
| --- | --- | --- | --- | --- |
| C1 | `tests/` → `tests/scripts/` + `tests/docs/` (группировка корня) | средняя (находимость, чистота) | средний (~35 ссылок: GNUmakefile, tests/CMakeLists.txt, .sourcecraft/ci.yaml, .github, skynet/*, SKILLS) | **Трогаем**, батч в фазе C; все ссылки в том же коммите |
| C2 | Доки: `tests/issues/` → `tests/ut/issues/` (пути в 8+ файлах) | средняя (онбординг, снижение ложных поисков) | нулевой (только текст доков) | **Трогаем** (микро-батч) |
| C3 | `.gitignore` vs `.codeassistant/` (противоречие) | низкая (гигиена) | нулевой | **Трогаем** (микро-батч) |
| C4 | Переименование тёзок `src/chk.c`/`src/defrag.c` | низкая (устранение путаницы) | высокая (upstream-паритет, constraint #2; правки 4+ build-файлов, история) | **НЕ трогаем**; опция — ADR владельцу |
| C5 | `src/api-*.c` гранулярность | — | — | **НЕ трогаем** (канон) |
| C6 | ChangeLog-*.md группировка | низкая | высокая (upstream, doxygen-конвейер) | **НЕ трогаем** |
| C7 | `skynet/` вынос из публичного репо | — | — | **НЕ трогаем** (вне scope; вопрос инфраструктуры) |
| C8 | `docs/` реструктуризация | — | высокая (upstream-канон) | **НЕ трогаем** |
| C9 | Man-страницы/`examples/`/`src/bits.md` | — | — | **НЕ трогаем** (канон, структура адекватна) |

---

## 4. Рекомендации для фаз B/C/D

1. **Фаза B** — детальная карта переносов только для C1–C3:
   - C1: `git mv` скриптов в `tests/scripts/`, доков в `tests/docs/`; полный
     список ссылок см. §F3 (проверить дополнительно `grep -rn` по всему репо
     на момент исполнения).
   - C2/C3: правки текстов доков и `.gitignore` — без `git mv`.
2. **Порядок батчей** (в окне без параллельных задач):
   - B1 = C2+C3 (микро, независимы);
   - B2 = C1 (основной, требует согласования с ревьюверами REV:cmake — т.к.
     затрагивает `.sourcecraft/ci.yaml`, `.github/workflows/*.yml`,
     `tests/CMakeLists.txt`, `GNUmakefile`).
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
  по содержанию, есть дрейф путей `tests/issues/`).
- Факты сборки подтверждены чтением `CMakeLists.txt` (строки 983, 993, 1069,
  1333-1363, 185-193, 67-73) и `GNUmakefile` (462-479, 540-545, 559-562, 959,
  979); артефакты `cmake-build-t41-check` (mdbx.dir/src, mdbx_chk.dir/src/tools).
- Итог: **дубликатов-кандидатов на удаление не найдено**; реальных кандидатов
  на перенос — 3 (C1–C3), все в нашем слое, upstream-набор не затрагивается.