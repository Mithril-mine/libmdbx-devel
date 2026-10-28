# SKILL: shared-graph-memory.mdbx — управление знаниями роя

> Операционная инструкция для агента. Схема хранилища и форматы —
> в [`docs/SCHEMA.md`](docs/SCHEMA.md), проектирование — [`docs/DESIGN.md`](docs/DESIGN.md).

## Назначение

shared-graph-memory.mdbx — персистентная память роя на libmdbx (ACID, MVCC).
Хранит **устойчивые результаты**: решения, баги, процессы, узкие места, факты.
**НЕ хранит промежуточные рассуждения.** MCP-инстанс регистрируется как
`shared-graph-memory.mdbx`; у агента инструменты выглядят как
`<server>_<tool>` (например `shared-graph-memory_mdbx_safe_store`).

- `safe_store` — единственный путь записи (атомарная дедупликация, 1 write-txn).
- `recall` / `search` / `lookup` / `graph` — чтение.
- Контролируемый словарь (`vocab`) — канонические модули и темы ключей.

## Инструменты (MCP tools)

| Инструмент | Назначение |
|---|---|
| `normalize_key(raw)` | Проверка/канонизация строки в `тип:модуль:тема` |
| `safe_store(key, type, summary, importance, date?)` | Сохранение с дедупом → `created`/`merged`/`conflict` |
| `recall(pattern, limit=5, touch=false)` | Поиск по префиксу (`bug:crypto:*`), ранжирование; touch=true — учёт чтения в LRU. Значение по умолчанию touch=false: чтение НЕ кормит recency — активно читаемая, но не помеченная запись со временем уйдёт в cold/gc. Передавайте touch=true, когда чтение = «использую запись» и её нужно удержать от gc |
| `search(query, limit=5, touch=false)` | Поиск по термам (пересечение постинг-списков), ранжированные записи; touch=true — учёт чтения в LRU (см. recall о touch) |
| `lookup(term)` | Точный поиск: AND-пересечение термов → ключи |
| `link(subject_key, predicate, object_key)` | Типизированная связь (RDF triple) |
| `unlink(subject_key, predicate, object_key)` | Удаление связи |
| `graph(key, depth=1)` | Обход графа связей |
| `vocab_add(module, topic?)` | Добавить модуль/тему в словарь |
| `vocab_find(query)` | Fuzzy-поиск по словарю |
| `exists(key)` | Проверка существования |
| `dump_context(...)` / `restore_context(context_id)` | Снимок/восстановление контекста. ВНИМАНИЕ: restore_context НЕ продлевает recency даже event-записей снимка (внутренние recall без touch); если event в снимке нужно удержать от gc — `touch(key)`. Два пути учёта использования: `touch(key)` — явная отметка (одна write-txn, без rate-limit); `recall(..., touch=true)` и `search(..., touch=true)` — пакетные (rate-limit 60с на запись; обе вызывают `_flush_touches()` — одна write-txn флэш на набор) |
| `gc(dry_run=true, archive=false)` | Тиринг hot/warm/cold (не удаляет без purge) |
| `purge(keys)` | Явное удаление записей |
| `stats()` | Статистика хранилища (здоровье памяти) |
| `db_status()` | Диагностика БД (`MDBX_envinfo`: меты/bootid/txnid/pgop/geo, sync-пороги) + `ops` (busy_retries/busy_failures/commit_p90_us — **per-process**, только счётчики своего процесса) |
| `db_flush()` | Принудительный сброс данных на диск (sync force=true) |
| `db_readers(check=false)` | Число активных читателей; check=true чистит мёртвых |
| `db_stat(table?)` | Статистика env или конкретной таблицы |
| `db_set_mode(sync?, readonly?)` | Ротация sync (durable\|metasync\|safe_nosync) и/или read-only↔rw |
| `db_enable_utterly_nosync()` | **ОПАСНО**: отключение синхронизации (см. §«Режимы БД») |
| `db_backup(skip?, force?)` | PIN-точка сохранения внутри backup-каталога (COMPACT, не ротируется); ответ на PROMPT-запрос |
| `db_export(dest)` | Экспорт копии НАРУЖУ каталога (внешняя архивация/перенос); не учитывается в лимите и tiers |
| `db_backup_status()` | Состояние бэкапов: таймеры, тиры (lifeline/auto/pin/snapshot), `auto_skipped_limit`, `auto_failure_count`, кандидаты ротации |
| `db_backup_cleanup(dry_run?, confirm?)` | Показать/удалить старейшие `auto-*` (только AUTO-тир) |
| `db_fileinfo()` | fstat(fd): st_nlink/size/inode + признак удаления файла БД |
| `db_latency()` | Commit-латентности последних записей по стадиям (µs) |
| `db_recover(target_meta?)` | Диагностика меты для восстановления (безопасный probe) |
| `map_symbol(key)` / `map_symbols(prefix?, limit?)` | Символ / префиксный список карты исходников |
| `map_edges_of(caller)` / `map_callers_of(callee)` | Вызовы / обратные рёбра (impact) |
| `map_group_members(group)` | Члены фасет-группы (`group:subsystem:{module}` и др.) |
| `map_regions(prefix?, limit?)` / `map_region(key)` | #if-дерево (region:{module}:{n}) |
| `map_uncovered(prefix?, limit?)` | Uncovered-острова смысла вне union-AST |
| `test_metrics_insert(test, duration_ms, labels?, timeout_sec?)` | Окно истории длительностей тест-кейса (минутное: last/min/max/p90, флаки) |
| `test_metrics_get(test)` / `test_metrics_all(prefix?)` | Метрики теста / всех |
| `coverage_insert(test, symbols, pct?, layer?)` / `coverage_batch(tests, layer?)` | Покрытие теста (набор fn:/type:/macro:), батч |
| `coverage_get(test, layer?)` / `coverage_all(layer?)` / `coverage_remove(test, layer?)` | Покрытие / все / удалить (чистит и coverage_blocks) |
| `coverage_symbols(symbol_prefix?, layer?)` | **Какие тесты защищают символ** (covered_by) |
| `coverage_blocks_insert(test, blocks, layer?)` / `coverage_blocks_get(test, symbol, layer?)` | Блочная детализация: каким блокам символа тест прошёл |
| `coverage_summary(module?, layer?)` | **Сводка покрытия**: union по модулям — covered/total символов+блоков, pct |
| `coverage_gaps(module?, layer?)` | **Белые пятна**: символы модуля без покрытия ни одним тестом |
| `ci_jobs_insert(job, body)` / `ci_jobs_all(prefix?)` | Параметры CI-джоб (`{workflow}/{job}`) |
| `ci_jobs_get(job)` / `ci_jobs_by_test(test)` | Одна джоба / джобы, исполняющие тест |
| `project_load(files, edges, replace?)` | Загрузка project-map (весь репозиторий) |
| `project_files(prefix?, limit?)` | Файлы проекта по префиксу пути |
| `file_deps(path)` / `file_dependents(path)` | Зависимости файла / зависящие от него |
| `probe_add(tag, kind, ...)` / `probe_batch(probes)` | Реестр зондов (SystemMap): регистрация, dedup по семантическому тегу |
| `probe_get(tag)` / `probe_list(prefix?, kind?)` | Зонд / список зондов |
| `probe_status(tag, status)` / `probe_remove(tag)` | Смена статуса / удаление |
| `trace_ingest(caller, callee, count?, label?)` | Наблюдаемый вызов (runtime), инкремент счётчика |
| `trace_ingest_batch(pairs, layer?)` | Батчевый ingest (одна txn) из дампа probe-bus |
| `trace_observed(caller?)` | Наблюдаемые вызовы (по префиксу caller) |
| `trace_path(src, dst, depth?)` | Пути по observed-рёбрам (BFS) |
| `trace_compare(symbol)` | **possible vs observed**: static (call_edges) vs runtime (traces) |
| `map_put_edge(caller, callee, kind?)` | Записать ребро вызова (syntax/semantic) |
| `map_put_symbol(key, body, layer?)` | Записать символ карты; `cpp_status` (none/mapped/partial/done) валидируется |
| `map_alias(old_key, new_key)` / `map_refresh_stale()` | Фиксировать переименование / перецепить висячие ссылки |
| `map_mark_stale(live_keys, layer?)` | Пометить отсутствующие символы stale (alive остальным) |
| `map_port_status(module?, layer?)` | **Приборная панель C→C++**: cpp_status по модулям + топ непортированных (по блокам) |
| `map_generations()` / `map_snapshot()` | Поколение карты (canary.y) / полный снимок |
| `map_diff(other?)` | Разница текущего снимка с прошлым (added/removed/common) |
| `adr_update(name, ...)` / `adr_get(name)` | ADR: сохранить/прочитать архитектурное решение |
| `adr_outline(prefix?)` / `adr_delete(name)` | Обзор ADR / удаление |
| `cbm_link(our_key, cbm_node, cbm_project?)` / `cbm_unlink(our_key)` | Мост: наш fq-ключ ↔ узел codebase-memory-mcp |
| `cbm_linked(prefix?)` | Список мостов |
| `cbm_search(query, project?, tool?, args?)` | Поиск в CBM (CLI-подпроцесс). **Опционально**: без CBM → `unavailable: true`; полноценная работа сохраняется |
| `version_info` | **Версии**: модуля / libmdbx / схемы данных / данных (map_generation, records, symbols) — для оперативного контроля состояния |

> ⚠️ **Зонды в базе знаний — только постоянные.** Временные/отладочные
> пробники (добавляемые при трассировке одной ветви) НЕ регистрируются в
> `probes`: если такой зонд всё же попал в реестр — удалите его
> (`probe_remove`) или перегенерируйте реестр свежим `scan_probes.py`
> (с замещением). Реестр — SystemMap для постоянных точек; мусор деградирует
> навигацию и поведенческую паритетность C→C++.

### Ветки и слои (нелинейное версионирование БЗ)

Параллельные ветки кода живут в **отдельных слоях** (идентификатор
`{ver}.{feature}.{n}`, см. docs/BRANCHES.md):

- `branch_register(ver, feature, n)` / `branch_list` / `branch_current` /
  `branch_activate(layer, expected_prev?)` — управление слоями;
  `expected_prev` — CAS-защита от гонки активаций в рое;
- `branch_remove(layer)` — убрать запись реестра; `branch_purge(layer,
  expected_current?)` — удалить слой **со всеми данными**
  (symbols/edges/coverage/probes/traces + sym_id-пары); базовый слой purge
  запрещён; АКТИВНЫЙ слой purge тоже запрещён — сначала переключите ветку
  (`branch_activate`), иначе `meta.branch_current` укажет на мёртвый слой;
  `expected_current` — CAS-защита окна гонки purge/activate (проверяется
  ВНУТРИ той же write-txn, атомарно): если активный слой не совпал с
  ожидаемым — `CONFLICT`;
- **Активация незарегистрированного слоя** → `warning`: «слоя никогда не
  было (опечатка?) ИЛИ он purge'нут другим агентом (данные не вернутся)».
  Для опечатки — исправьте slug; для purge'нутого — пересоздайте с нуля
  (branch_register не восстановит данные);
- `map_diff_branches(from, to)` — **структурный diff по содержимому** карты
  (не по коммитам): что реально изменилось между ветками;
- `coverage_migrate(from, to, symbols?)` — копия coverage/blocks/traces
  слоя в другой (merge ветки): источник цел, конфликт test_id/caller →
  перезапись (для coverage — суженная фильтром; для traces — caller
  копируется целиком телом источника). Битые/несогласованные записи
  источника пропускаются с отчётом `skipped`;
- `layer=` принимают map-инструменты (`map_symbol(s)`, `map_edges_of`,
  `map_callers_of`); базовый слой (`0.15`) — без префикса;
- **Правило**: в feature-ветке работайте в СВОЁМ слое (передавайте `layer`),
  не затирая базовый. Знания (curated-слой) — общие; структурный слой —
  per-branch.
- **Активная ветка**: после `branch_activate(layer)` все инструменты о
  состоянии кода — `map_*`, `coverage_*` (insert/batch/remove/get/all/
  symbols/summary/gaps/blocks), `probe_*`, `trace_*` — по умолчанию
  читают/пишут ЭТОТ слой; аргумент `layer` перебивает активную ветку.
  Коллекторы (`collect_coverage.py`, `gcov_to_coverage.py`, `load_map.py`)
  принимают `--layer`. Знания (curated-слой: records/links/vocab/adr/
  ci_jobs/test_metrics) — общие, per-branch только структурный слой.
- **Иерархия слоя** (кто побеждает при `layer=""`): явный аргумент `layer=`
  > env `SHARED_GRAPH_MEMORY_LAYER` (per-process, читается один раз) >
  `meta.branch_current` (глобальный). В рое из нескольких агентов каждая
  ветвь выставляет свой `SHARED_GRAPH_MEMORY_LAYER` — и чужой
  `branch_activate` не переключит её записи в чужой слой.
  Исключения (глобальные, БЕЗ слойной изоляции): `map_alias`
  (таблица `sym_aliases`) и `map_refresh_stale` (сканирует `sym_id2key`/
  `links` целиком) — переименования и чистка висячих ссылок видны всем
  слоям. Curated-знания (records/links/vocab/adr) — тоже общие.
- При слиянии: `map_diff_branches(base, feature)` → перенос новых символов в
  base → `coverage_migrate(feature, base)` → пересканировать base → feature
  `status=merged`; мёртвый слой после rebase — `branch_purge(feature)`.

### Совместимость схемы данных (SCHEMA_VERSION)

- `version_info` → `schema_matches` + направление: `schema_too_new`
  (БД создана более новой версией модуля — **обновите MCP-модуль**, иначе
  `Store` откажется открывать БД ошибкой `SCHEMA_TOO_NEW`) / `schema_outdated`
  (БД старше модуля — методы опциональных таблиц дают `SCHEMA_OUTDATED`,
  «переключитесь в rw-режим (`db_set_mode readonly=false`) для миграции»).
- КОНТРАКТ `SCHEMA_OUTDATED` — ЕДИНЫЙ для всех методов опциональных таблиц
  (branch_*, coverage_*, probe_*, trace_*, test_metrics, ci_jobs): на старой
  схеме они бросают ошибку, а НЕ возвращают `{}`/`[]` (иначе «пусто = нет
  данных» — молчаливая потеря). На актуальной схеме + read-only старт
  (таблица не создана) — прежняя толерантность (пустой результат).
- Старая схема репозитория (schema_ver=1) — breaking change: `branch_list`
  больше не `[]`, а `SCHEMA_OUTDATED`; миграция — переход в rw.

### Резервные копии — контракты

- `db_backup` (pin-*) / `db_export` (наружу, dest обязателен ВНЕ каталога
  включая подкаталоги — realpath+normcase) / auto-таймер.
- ПЕРСИСТЕНТНЫЙ флаг `auto_skipped_limit` в `.backup-state.json` STATE
  (виден рою, переживает рестарт): ставится при лимитной причине (INVALID),
  снимается успехом auto ИЛИ cleanup при восстановлении места (пин-only
  переполнение — ручным rm + cleanup). State-файл: merge-запись под
  блокировкой (flock/msvcrt) — параллельный процесс не затирает флаг.
- Таймер авто: `last_auto` продвигается на успехе/осознанном пропуске
  лимита; при НЕ-лимитной ошибке — `last_auto` ОТОДВИГАЕТСЯ на
  экспоненциальный backoff (`interval*2^count`, потолок 32x; счётчик
  `auto_failure_count` в state-файле, общий для РОЯ — per-key merge под локом),
  попытка реже, не затихание. Флаг `auto_skipped_limit` виден даже в окне
  backoff (проверка размера каталога до таймер-гейта). Рой при ENOSPC не
  повторяет снапшот ~ГБ каждый тик. `backup_status` показывает
  `auto_failure_count` (текущий backoff-счётчик) и корректный `next_auto`:
  = `last_auto + interval` в обоих режимах (без backoff — попытка через
  интервал после успеха; при backoff last_auto = now + interval*2^count,
  гейт допускает now >= last_auto + interval — реальный момент попытки
  через last_auto + interval). Потолок backoff 32·interval; с учётом гейта
  попытка — примерно 33·interval от последнего тика.
- `__future__`: `db_readers check=true` — ручная чистка stale-слотов; рой
  без rw-писателя — явные `check=true` (libmdbx чистит читателей сам).

### Карта исходников (refactoring-map)

- Структурный слой: `symbols` (fn:/type:/macro:), `call_edges(+rev)`, `groups`,
  `regions`, `uncovered`; наполняется `scan_symbols.py` → `load_map.py`.
- **Ключи**: канонические fq-ключи функций — `fn:{qname}` (module в теле,
  `@<module>` для TU-static, `#<sig>` для перегрузок). **Не выдумывайте
  ключи**: получайте их через `map_symbols(prefix)` / `map_symbol` перед
  `map_edges_of`/`map_callers_of`/`coverage_symbols`. Поиск по модулю —
  `map_symbols("fn:gc")` (фильтр по module-полю).
  `regions` (#if-дерево), `uncovered` (острова смысла вне union-AST всех конфигов).
- Данные генерируются `tools/scan_symbols.py` (+scan_regions/uncovered),
  загружаются `tools/load_map.py --db <живая> --replace`, сверяются
  `tools/validate_map.py` (golden: «не меньше»).
- **Многоплатформенность**: полная карта = merge linux+win32+macos.
  Локально конвейер воспроизводится одной командой
  `python3 tools/scan_all.py --configs linux,win32 [--db <живая>]`
  (macos-артефакт — из CI). Подробности и root-cause заметки — в
  `docs/REFACTORING-MAP.md`, секция «Многоплатформенное сканирование».
- Использование для рефакторинга: `map_edges_of('fn:x')` — кто вызывается;
  `map_callers_of('fn:x')` — impact; `map_uncovered('api-copy')` — непокрытые
  острова в модуле; `map_regions('region:chk')` — платформенные ветки.
- Перегенерация/смена кода: ссылки на исчезнувшие символы чинятся
  `map_refresh_stale()`.

#### Динамическое покрытие (coverage layer)

Статический `uncovered` показывает *структурную* изоляцию (острова вне
union-AST), но не отвечает на вопрос «что реально выполняется тестами».
Для этого используется слой динамического покрытия (lcov/gcov):

- Источник: GitHub клетка `linux-gcc-ubuntu24-debug-coverage`
  (артефакт `coverage-lines.info`, `lcov --include src/`); SourceCraft
  `ci-linux-nightly-full` печатает summary в лог куба.
- Хранение: одна запись на прогон
  `fact:testing:coverage-<branch>` (важность 0.3), внутри — дата, общий
  % строк/функций и список «худших» файлов (<55% строк) с процентами.
- Интерпретация: `fact:testing:coverage-*` + `map_uncovered('<модуль>')`
  вместе дают карту недотестированности — структурные дыры (uncovered)
  поверх количественных цифр. Приоритет новых тестов:
  1) худшие покрытые модули (`api-cursor`, `mdbx.c++`, `coherency`,
     `api-cold`, `logging_and_debug`, `mvcc-readers`);
  2) пересечение статического uncovered и низкого % (и там, и там дыра —
     либо код мёртвый, либо нужен тест).

### Формат ключа

`тип:модуль:тема` — ровно 3 сегмента.
- **Типы**: `decision`, `bug`, `proc`, `bottleneck`, `fact`, `event`.
- **Модуль** — из словаря (см. §«Схема знаний»); новый модуль/тема — только
  через `vocab_add` (сначала `vocab_find`).
- **Тема**: kebab-case, ≤ 30 символов, 1–3 слова, **без цифр/версий/дат**
  (например `alignment`, но НЕ `alignment-arm64` — «arm64» отбрасывается).

### Допустимые предикаты `link`

Базовый набор: `caused-by`, `fixed-by`, related-to`, `supersedes`, `part-of`,
`depends-on`.
Контекстные предикаты (для глубокого рефакторинга и покрытия):
`covers` (запись о тесте покрывает символ/функцию), `implements` (С++-метод
реализует C-функцию), `invalidates` (изменение кода делает запись устаревшей),
`renders-obsolete` (символ делает другой устаревшим), `backported-to`
(фикс перенесён в ветку), `migrated-from` (код переехал из модуля X).

**Схема предикатов открыта**: любые литералы без `:` и NUL-байта допустимы
(реализация не накладывает закрытый список; расширение указывается в этом
документе, чтобы рой использовал один словарь).

## Схема знаний (домены)

Домен = группа модулей словаря. Домен кладётся в **модуль** ключа, поэтому
протоколы роя не блокируют общие практики, а знания проекта всегда «под рукой».

| Домен | Модули | Содержимое | Где доки |
|---|---|---|---|
| **0. meta** (нулевой, обязательный) | `meta` | знания о знаниях: карта, схема, процедуры, самонаблюдения double-loop | master |
| **1. general** | `practice` | общие практики и принципы (Кайдзен, методологии) | master |
| **2. swarm** | `swarm`, `coordination` | протоколы роя, роли, координация | skynet-draft |
| **3. project** | `crypto`, `network`, `storage`, `ui`, `build`, `testing`, `review`, `platform`, `core`, `memory` | архитектура, инварианты, ловушки libmdbx | master |
| **4. todo** | `todo` | неклассифицированное, ждёт разбора | — |
| **5. archive** | `archive` | устаревшее/legacy, хранится для справки | — |

Действующие правила (записаны в `fact:meta:knowledge-map`):
- Внутри devel/skynet-draft ничего не меняем; полезное оттуда извлекаем и
  храним рядом — в этой памяти.
- Знания преимущественно про master; бэкпорты — по месту (каждый фикс —
  отдельная ситуация, тяжёлое управление не нужно).

**Рецепты выборок:**
- Память в целом: `recall("fact:meta:*")` — карта знаний.
- Практики: `recall("proc:practice:*")`; Кайдзен-принципы: `search("kaizen")`.
- Рой: `recall("fact:swarm:*")`, `recall("proc:coordination:*")`.
- Проект: `recall("fact:{модуль}:*")`, `search("<термин>")`.
- Неясное/отложенное: `recall("fact:todo:*")`; устаревшее: `recall("fact:archive:*")`.

### Поиск: BM25-ранжирование и структурный слой

`search(query)` ранжирует по запросной релевантности (IDF редких терминов
проекта — `pnl`, `bigfoot`, `rkl`, `troika` — значимо выше частых) поверх
композитного score записи. При совпадении с идентификатором символа/фаcеты
(`fn:...`, `type:...`, `macro:...`, `group:...`) в выдачу добавляются элементы
**структурного слоя** (`kind="symbol"`/`group`): единая точка входа «код +
знания» для исследования кода при рефакторинге.

## Режимы БД (read-only, sync)

- **Сервер стартует read-only** по умолчанию. Для ЗАПИСИ сначала вызовите
  `db_set_mode(readonly=false)`; после работы можно вернуть read-only
  (`readonly=true`). Это защищает память от случайных/двойных write-агентов.
- **sync-режим** переключается на лету: `db_set_mode(sync="durable")` —
  максимальная надёжность, `metasync`, `safe_nosync` (по умолчанию; при крахе
  процесса данные в page cache, целостность БД гарантирована).
- `db_flush()` — принудительный сброс на диск (после критичных записей).
- **`db_enable_utterly_nosync` НЕ вызывать** без крайней необходимости и явного
  указания: он снимает гарантии durability — после краха последние транзакции
  теряются безвозвратно. Только для одноразовых кэшей. Выход из него —
  `db_set_mode(sync="safe_nosync")`.
- Если `db_status()` показывает unsteady меты (или открытие упало с
  «recovery needed») — не делайте «молчаливых» операций: решите явно —
  `mdbx_chk` или осознанное открытие read-write (движок сам сделает steady-sync).

## Бэкап и защита от удаления файла БД

- **LIFELINE** (`current.mdbx`, жёсткая ссылка) создаётся при старте сервера;
  инвариант `st_nlink ≥ 2`. **EMERGENCY** (`snapshot_*`) — при детекции
  удаления файла БД. Эти тиры НИКОГДА не удаляются автоматически.
- **AUTO** (`auto-*`) — тихий интервал, ротируется; **PIN** (`pin-*`) —
  закреплённая точка сохранения внутри каталога (`db_backup`, PROMPT-запрос
  или явный вызов), учитывается в лимите, НЕ ротируется; **EXPORT** —
  копия НАРУЖУ каталога (`db_export dest=…`, внешняя архивация/перенос),
  не учитывается в лимите и tiers.
- **Получив `DB_FILE_DELETED`** — выдать пользователю ПРЕДУПРЕЖДЕНИЕ и спросить:
  (1) восстановить файл жёсткой ссылкой из `backup_dir`
  (например `os.link("<backup_dir>/current.mdbx", "<path>")`), (2) остановить
  сервер для `mdbx_chk`, или (3) продолжить, осознав риск. Операции записи
  заблокированы до решения.
- **Получив notice `backup_pending`** — спросить пользователя: «сделать
  резервную копию сейчас?» → да: `db_backup` (pin-точка); нет:
  `db_backup(skip=true)`.
- **Ротация AUTO**: `db_backup_cleanup(dry_run=true)` показывает кандидатов
  (старейшие сверх `BACKUP_KEEP`). Удаление — ТОЛЬКО после подтверждения
  пользователя, через `confirm=true`. Тиры pin/lifeline/snapshot ротация
  не трогает.
- **Лимит каталога** (`BACKUP_MAX_MB`): pin/auto сверх лимита → явная
  ошибка (причина + remedy: cleanup / поднять лимит / `force=true`);
  auto-пропуск ставит персистентный флаг `auto_skipped_limit` (виден в
  `db_backup_status`). Export наружу лимиту не подчинён.
- После критичных записей — `db_flush()`; плановый бэкап — `db_backup`
  (pin-точка); копия для внешней архивации/переноса — `db_export(dest=…)`.
- **State-файл и flock (claim/reserve, Б3)**: `.backup-state.json` —
  per-key merge под коротким `_file_lock`. AUTO-копия (~ГБ) выполняется
  ВНЕ лока: под локом только claim (`last_auto=now`) и finalize. Рой НЕ
  блокируется на время копии; дубль копии исключён claim'ом (второй агент
  видит свежий `last_auto` → гейт закрыт). Crash между claim и finalize
  стоит один interval.
- **Деградация лока (Q8/R2)**: если `_file_lock` недоступен (NFS без
  lockd, EINTR), merge-семантика деградирует до lockless — это ЗАМЕТНО:
  `warnings.warn(RuntimeWarning)` + `backup_notice` «state lock
  недоступен (NFS/сигнал): merge-семантика …lockless». Ничего чинить не
  надо (следующий успешный лок вернёт семантику); notice информативный.
  `_file_lock` реентерабелен (вложенный save на тот же файл — yield).

## Когда сохранять

| Событие | Тип | Важность |
|---|---|---|
| Принято архитектурное решение | `decision` | 0.9 |
| Найден баг, есть воспроизводимая причина | `bug` | 0.7 |
| Выработан или обновлён процесс | `proc` | 0.8 |
| Обнаружено узкое место процесса (не кода) | `bottleneck` | 0.7 |
| Установлен постоянный факт о проекте | `fact` | 0.9 |
| Значимое событие (баг найден, фикс применён) | `event` | 0.5 |
| Coverage-прогон с устойчивым результатом (проценты + худшие файлы) | `fact` | 0.3 |
| Длительности/флаки теста (окно истории, коллектор ctest XML) | `test_metrics_insert` | — |
| Покрытие теста (набор символов) — основной слой «тест → код» | `coverage_insert` | — |

- Тип — часть ключа и фильтра recall: `{тип}:{модуль}:*`.
- Важность нормируется в [0,1]. Низкая важность (≈0.1) у `archive`,
  средняя (≈0.4) у `todo` — чтобы не шуметь в recall.
- После `safe_store` для связанных записей вызвать `link`.

## Когда НЕ сохранять (muda)

- Промежуточные вычисления и ход рассуждений.
- Одноразовые факты (применимы только к текущей задаче).
- То, что уже есть в коде, документации или артефактах.
- Дубликаты — `safe_store` не даст создать дубль, не пытайся обойти это.
- Каждый цикл PDCA — только когда есть новый устойчивый результат.

## Когда доставать

| Ситуация | Что вызывать |
|---|---|
| Перед новой задачей | `recall` по теме/домену задачи (1 вызов) |
| При ошибке/баге | `recall("bug:{модуль}:*")` |
| При вопросе «как делать X» | `recall("proc:practice:*")` или `lookup("X")` |
| Перед архитектурным решением | `recall("decision:{область}:*")` |
| Когда точный поиск не дал результата | `search("запрос")` |
| Нужен контекст записи | `graph(key, depth=2)` |
| Проверить здоровье памяти | `stats()` |
| После сжатия контекста | `restore_context(context_id)` (вернёт последний снимок) |
| Перед сжатием контекста | `dump_context(task=..., milestone=...)` |

Лимиты: не чаще 1 retrieval на шаг; не больше 5 записей за раз. Если 3 попытки
подряд не дали результата — прекрати искать, действуй из контекста.

## Процедура PDCA для работы со знаниями

1. **Plan** — на старте задачи: один `recall`/`search` по теме; реши, что
   заслуживает фиксации после завершения.
2. **Do** — после завершённого проверяемого результата: `safe_store` + `link`;
   при изменении — обновить существующую запись (`safe_store` того же ключа).
3. **Check** — при следующем обращении: если запись устарела или противоречит
   коду — **поправить сразу** (доверяй коду, Gemba).
4. **Act** — мета-шаг double-loop (ниже).

## Double-loop (непрерывное усовершенствование на основе опыта)

После каждой **значимой задачи** — один обязательный мета-шаг:

1. Помогла ли память (нашлось нужное, быстро)?
2. Что было не так: пустые/ложные выборки, неверный тип/модуль, пропущенный
   синоним, избыточная запись?
3. Что изменить: словарь (`vocab_add`/`remove_topic`), важность, тип, сам SKILL?

Вывод — запись-самонаблюдение в `proc:meta:*` (например
`proc:meta:usage-alignment`). Если менять нужно **схему/процедуру** (не отдельную
запись) — предложи владельцу: это влияет на весь рой.

Так петля замыкается: single-loop исправляет ошибки в существующих правилах,
double-loop улучшает сами правила на накопленном опыте.

## Карта Кайдзен

| Принцип | Применение в работе с памятью |
|---|---|
| **Gemba** («пойди и посмотри») | Любое наблюдение сверять с кодом перед действием; при расхождении — доверять коду и править запись |
| **PDCA** | Цикл из §«Процедура PDCA» |
| **Малые шаги** | Компактные факты с provenance, атомарные правки |
| **Muda** (устранение потерь) | Не писать одноразовое, не дублировать, не пересканировать свежую память |
| **5 Whys** | Фиксировать инварианты и причины, не только «что» |
| **Стандартизация** | Единый формат ключа и словарь; стандарт сам улучшается через double-loop |
| **Измерение** | Отмечать хит/мисс retrieval и stale-правки; учитывать в мета-шаге |

## Контракт ошибок

Ошибки возвращаются строкой:

```
error$<CODE> | CLASS=<class> | DESC=<human> | ACTION=<hint> | RETRY=<policy>
```

| CLASS | Ретраи | Действие агента |
|---|---|---|
| `invalid` | none | Исправить запрос: проверить ключ/словарь (`vocab_find`, затем `vocab_add`), арность `тип:модуль:тема` |
| `size-limit` | none | Сократить данные или разбить на несколько записей |
| `busy-io` | до 3 (авто) | Просто повторить операцию |
| `internal` | 1 + эскалация | Сообщить оператору |

`ACTION` — рекомендация, не директива.

## Экономия токенов

1. Суть записи — max 3 предложения. Не помещается — разбей на несколько записей.
2. Не вызывай `normalize_key` перед `safe_store` — она нормализует сама.
3. Не вызывай `exists` перед `safe_store` — она проверяет сама.
4. При retrieval — конкретный тип и домен/модуль, не «дай всё».
5. Достал запись и не помогла — не доставай ещё. Сделай шаг без памяти.
6. Не сохраняй в каждом цикле PDCA. Только устойчивый результат.
7. Не пиши связи в теле записи — используй `link`.
8. Перед `vocab_add` — `vocab_find`: терм может уже быть под синонимом.
9. Используй домен в ключе — он же фильтр выборки.
10. Проверку здоровья — одним `stats()`, без dump.

## Стоп-условия

- 3 попытки retrieval подряд без результата → действуй из контекста.
- `safe_store` вернул `conflict` → реши за один шаг: обновить существующую или
  создать новую (не углубляйся).
- Запись противоречит коду → доверься коду, обнови запись.
- `gc` возвращает `cold > 50` → сообщи человеку (порог высок или мусора много).

## Тирирование (hot / warm / cold)

Композитный score `0.35·importance + 0.25·recency + 0.20·access + 0.20·links`:

| Тир | Условие | Поведение |
|---|---|---|
| Hot | score > 0.7 | Не трогается |
| Warm | 0.3 < score ≤ 0.7 | Оставляется |
| Cold | score ≤ 0.3 | Кандидат на архив/удаление |

`gc` классифицирует и (по запросу) архивирует cold; **сам записи не удаляет** —
только явный `purge`.

## Пример: «Нашёл баг»

```
1. recall("bug:crypto:*") — есть ли похожая запись; иначе search("segfault")
2. safe_store("bug:crypto:alignment", "bug", "Segfault AES-CBC ...", 0.7)
   → conflict? тогда решить: обновить существующую или новый ключ
3. link("bug:crypto:alignment", "related-to", "fact:platform:android")
4. safe_store("event:crypto:bug-found-alignment", "event", "...", 0.5)
5. link(event, "related-to", bug)
```

Заметка: тема без цифр/версий — «alignment», а не «alignment-arm64» (слова с
цифрами нормализация отбрасывает).

## Пример: восстановление контекста после сжатия

```
1. dump_context(task="TASK-42", milestone="M1",
                keys=["bug:crypto:alignment"], hypotheses=["h1", "h2"])
   → запоминаем context_id
2. ... продолжаем работу ...
3. restore_context(context_id) — вернёт последний снимок контекста
```