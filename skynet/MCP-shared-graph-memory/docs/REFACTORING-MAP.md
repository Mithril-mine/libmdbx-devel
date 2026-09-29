# REFACTORING-MAP — структурный слой: карта исходного кода для C→C++ и тестов/CI

> **Статус:** черновик (draft). Решения владельца: 2026-09-25.
> Консультация Алисы: `proc:practice:alice-consult-schema` (память).
> Итоговое решение: `decision:meta:schema-refactor-map` (память).
> Связано: SCHEMA.md §1.1 «Расширение схемы», BACKLOG B64/B65.

## Цель

Держать в той же персистентной памяти роя **структурную карту исходного кода**,
достаточную для:
- **тяжёлого рефакторинга**: перевод проекта с C на C++ с сохранением поведения
  (поведенческая паритетность), понимание «что на что влияет»;
- **реорганизации тестов и CI**: какие тесты что покрывают, что медленно/флаки,
  как распределить по джобам.

Урок прошлой итерации (архив старого mcp): сущности были грубые (уровень модуля),
наблюдения — свободный текст без структуры; нельзя было ответить «что вызывает X»,
«где реализован Y», «какие тесты защищают Z». Новый слой — **структурно, точно,
перегенерируемо**, с фаcетами и анкорами.

## Принципы

1. **Аддитивность**: только новые таблицы через `Store.TABLES`; существующий
   курируемый слой (`records`/`links`/`vocab`) не меняется.
2. **Перегенерируемость**: symbols/call_edges/coverage/test_metrics/ci_jobs
   заполняются детерминированными автогенераторами из исходников и прогонов;
   регенерация после каждого коммита самоизлечивает дрейф координат.
3. **Устойчивые идентификаторы**: привязка к символу и структурному блоку
   (`symbol#block-id`), а не к номерам строк; строки — перегенерируемые метаданные.
4. **Мост с курируемым слоем**: `link(record, "related-to", "fn:...")`;
   ссылки на исчезнувшие символы помечаются `stale` при регенерации.
5. **Единый API доступа/обновления** к обоим слоям — против рассинхрона.

## Таблицы (аддитивные)

| Таблица | Флаги | Ключ → Значение |
|---|---|---|
| `symbols` | DEFAULTS | `fq-имя символа` → JSON (ниже) |
| `call_edges` | DUPSORT | `caller_id` → JSON `{kind, resolved, ambiguous, callee}` |
| `call_edges_rev` | DUPSORT | `callee_id` → `caller_id` (обратные рёбра для impact) |
| `groups` | DUPSORT | `group:{kind}:{name}` → set member_key |
| `sym_ids` | DEFAULTS | symbol_key → uint64 id (стабильный, не чистится) |
| `sym_id2key` | INTEGERKEY | uint64 id → symbol_key (для links/моста) |
| `probes` | DEFAULTS | `probe:{symbol}#{block-id}` → JSON |
| `coverage` | DUPSORT | `test_id` → set symbol_id |
| `ci_jobs` | DEFAULTS | `{workflow}/{job}` → JSON |
| `test_metrics` | DEFAULTS | `test_id` → JSON (окно истории) |

### symbols

```json
{
  "kind": "function | type | macro | variable | file",
  "signature": "static int mdbx_env_open(MDBX_env *env, ...)",
  "file": "src/api-env.c",
  "module": "api-env",
  "l0": 123, "l1": 245,
  "blocks": [{"id": "B2", "l0": 140, "l1": 175, "kind": "loop|branch|subcall|try|..."}],
  "cpp_status": "none | mapped | partial | done",
  "cpp_eq": "mdbx::env::open"
}
```

- **fq-имя**: `fn:api-env:mdbx_env_open` (для функций), `type:...`, `macro:...`.
- **Идентичность блока** — структурная (`symbol#block-id`), номера строк
  (`l0/l1`) перегенерируются сканером и могут дрейфовать без потери связей.
- `cpp_status`/`cpp_eq` — статус миграции C→C++; наполняется курируемо
  (агенты помечают при работе) + стартовые эвристики из карты соответствий.

#### Многоконфигурационные символы (`implementations`)

Один логический символ может иметь разные тела в разных конфигурациях
сборки (например `lck_seize`: posix в `lck-posix.c`, win32 в
`lck-windows.c`). При слиянии per-config артефактов (`scan_symbols.py
--merge`) такой символ получает общие поля наверху (`kind/name/signature/
configs/in_configs`) и список тел:

```json
{
  "kind": "function",
  "name": "lck_seize",
  "signature": "int (MDBX_env *)",
  "configs": ["linux", "win32"],
  "in_configs": ["linux", "win32"],
  "implementations": [
    {"config": "linux", "file": "src/lck-posix.c", "module": "lck-posix",
     "l0": 280, "l1": 399, "blocks": [...], "regions": [...]},
    {"config": "win32", "file": "src/lck-windows.c", "module": "lck-windows",
     "l0": 421, "l1": 459, "blocks": [...], "regions": [...]}
  ]
}
```

Правила (решение владельца + консультация Алисы
`proc:practice:alice-consult-implementations`):
- символ в одном конфиге остаётся плоским (без `implementations`);
- идентичное тело (та же `file+l0+l1`) в нескольких конфигах не
  дублируется — только `in_configs` расширяется;
- разные тела — список `implementations`; блоки переносятся внутрь
  каждого тела (поэтому block-id неймспейсены конфигом);
- `configs` наверху = union конфигураций тел; `regions` связываются для
  каждого тела отдельно.

Рёбра в merged-артефакте дедуплицируются до уникальных пар
`caller→callee` (impact-анализ), поэтому `counts.edges` может быть меньше
суммы по конфигам.

### call_edges

```json
{"kind": "syntax | semantic", "caller_block": "fn:api-env:mdbx_env_open#B5"}
```
- `syntax` — фактические вызовы (из сканера);
- `semantic` — «реализуют одну функциональность / параллельны» (курируемо или
  из анализаторов) — решено добавить **сразу**.

### groups (фасеты)

`group:{kind}:{name}` → set ключей любого типа (символы, тесты, записи знаний, доки).
Kind: `subsystem | domain | theme | problem | task | todo | mechanism`.
Примеры:
```
group:subsystem:gc        → {fn:gc-get:..., fn:gc-put:..., test:ut-gc-put, fact:meta:...}
group:task:B63            → {символы, тесты, записи, доки по задаче}
```
Транверсальный запрос «всё, что трогает GC» = union групп по префиксу.

### probes (SystemMap — карта инструментирования)

```json
{
  "kind": "usdt | bookmark | injected",
  "status": "candidate | inserted | validated",
  "mechanism": "gc",
  "test_theme": "gc-put",
  "symbol": "fn:gc-put#B5"
}
```
- Точки, куда можно внедрить/внедрены трассировочные пробники (USDT/SystemTap)
  и «закладки»; привязка к механикам/подсистемам и темам тестов.
- Ценность для C→C++: сравнение трасс до/после переписывания — поведенческая
  паритетность.
- Источники реестра: `skynet/probes.md` (devel), контур B3/B17b.

### coverage, ci_jobs, test_metrics

- `coverage`: тест → какие символы покрывает (из коллектора).
- `ci_jobs`: джоба → матрица/платформа/набор тестов (из workflow-файлов).
- `test_metrics`: **окно истории** (решено владельцем):
  ```json
  {"last_ms": 1200, "min_ms": 900, "max_ms": 2500, "p90_ms": 1800,
   "runs": 12, "flaky_count": 2, "timeout_sec": 60, "labels": ["ut.gc"]}
  ```
  Не чистый снапшот — история последних N прогонов (коллектор из ctest/CI).

## Автогенераторы (tools/)

| Инструмент | Наполняет | Вход |
|---|---|---|
| `scan_symbols.py --config` | symbols, call_edges(syntax), per-config | `src/` (clang AST) |
| `scan_symbols.py --merge` | merged symbols (`implementations`) | per-config артефакты |
| `scan_symbols.py --tests` | тестовые символы, рёбра тест→lib | `tests/{ut,issues,framework}` |
| `scan_uncovered.py` | `uncovered` + `coverage_pct` | merged артефакт + regions |
| `collect_coverage.py` | coverage | ctest/CI-артефакты |
| `collect_test_metrics.py` | test_metrics | ctest XML / LastTest.log / CI |
| `map_ci_jobs.py` | ci_jobs | `.github/workflows/*.yml` |
| `refresh_stale.py` | пометка `stale` | diff между генерациями |

Паттерн исполнения — как `tools/build_libmdbx.py`; детерминированный прогон,
сверка счётчиков с эталонным подмножеством (валидатор генератора).

## Дорожная карта

1. **Шаг 0 (готово)**: фиксация контекста (этот документ, память, BACKLOG B65).
2. **Шаг 1 (пилот — реализован полностью)**:
   - `scan_symbols.py`: C + C++ (функции/типы/макросы/блоки/рёбра),
     clang ast-dump=json + `-E -dD` для макросов; **канонизация ключей**
     `fn:{qname}` (+`#sig` перегрузки, `@module` TU-static, дедуп inline);
   - `scan_regions.py`: **#if-дерево** (cond_regions) без компиляции +
     классификация (platform/option/technical, inner-function/gating) +
     визуализация `--tree`; привязка символов к регионам + фаcет `configs`;
   - `validate_map.py`: сверка со статичным golden (`tests/golden/`);
   - таблицы symbols/call_edges(+rev)/groups/sym_ids/sym_id2key + батчевый
     загрузчик `load_map.py --replace` (самоизлечение дрейфа);
   - **мост curated ↔ structural**: `link()` принимает символы
     (`fn:`/`type:`/`macro:`), id через `mdbx_dbi_sequence`, `refresh_stale`,
     `mdbx_canary_get/put` (magic/pоколение карты).
   Текущие счётчики (Linux x86_64, clang 18.1.3, обновлено 29.09 после
   порта C++ API/USDT-проб): functions 3309, types 376, macros 511,
   blocks 31860, edges 20805 (unresolved 1100 — системные вызовы/
   `__builtin_*`), regions 1133.
   (C++-перегрузки учитываются: один qname + сигнатура → `#sig-hash`.)
   Подготовка `_build-scan` (важно, иначе сканер молча теряет C-ядро):
   `cmake -S . -B _build-scan -DCMAKE_BUILD_TYPE=Debug
    -DMDBX_BUILD_CXX=ON -DMDBX_ENABLE_TESTS=ON -DMDBX_BUILD_TOOLS=ON
    -DINTERPROCEDURAL_OPTIMIZATION=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DMDBX_ALLOY_BUILD=ON -DMDBX_USE_MINCORE=OFF`
   — `MDBX_ALLOY_BUILD=ON` обязателен: `select_tus` ждёт TU `src/alloy.c`
     в compile_commands (в Debug по умолчанию OFF, т.к. библиотека
     собирается из split-файлов, и C-ядро выпадает из скана: functions
     падают ~3127→2400, blocks ~31213→4067);
   - `MDBX_USE_MINCORE=OFF` обязателен: linux-конфиг выставляет его в 1,
     а для win32-target mingw не объявляет `mincore()` → alloy.c не
     компилируется под `--target=x86_64-w64-windows-gnu`.
4. **Шаг 1b (многоконфигурационный скан + uncovered — реализовано)**:
   - `scan_symbols.py --config NAME [--target ... --defines ... --includes ...
     --extra-flags ...]`: кросс-конфигурационный прогон (win32 через
     mingw/clang `--target=x86_64-w64-windows-gnu`); из compile_commands
     сохраняются только `-D/-I`/`-isystem`, остальное собирается из
     аргументов;
   - `--merge ARTIFACT...`: слияние per-config артефактов в один —
     общие поля наверху + `implementations: [{config, file, module, l0,
     l1, blocks, regions, configs}]` для символов с телами в нескольких
     конфигурациях; идентичное тело не дублируется (`in_configs`);
     рёбра дедуплицируются (unique caller→callee);
   - `scan_uncovered.py`: «острова смысла» — строки кода вне union-AST
     всех конфигов, классификация по #if-регионам, метрика `coverage_pct`;
   - тестовые домены: `scan_symbols.py --tests {ut,issues,framework}`
     (синтетические TU, `-DMDBX_BUILD_TEST=1`, без detailed-pp для
     framework); рёбра тест→lib резолвятся через библиотечный артефакт
     (coverage seed). googletest-include берётся из реальных тестовых TU
     compile_commands (`-isystem .../googletest/...`), в cross-режиме
     `-isystem` сохраняется (иначе issues-домен не находит gtest.h).
   Многоконфигурационные счётчики (linux+win32 merged, 29.09):
   functions 4448, types 406, macros 573, blocks 34376, edges 12110
   (unique pairs), unresolved 1295, coverage_pct 96.09%, uncovered 443
   островов. Тестовые домены: ut 2503 fn / issues 1936 fn /
   framework 2302 fn.
5. **Шаг 2**: semantic-рёбра, реестр probes (из `skynet/probes.md`), коллекторы
   coverage/test_metrics/ci_jobs, полноценный coverage от mdbx_test
   (instrumented run), macOS-скан (нужен osxcross/SDK).

## Ограничения (зафиксированы)

- **Тела функций из веток, неактивных в текущей конфигурации, отсутствуют
  в AST** (например `lck-windows.c` на Linux): `configs`-разметка честно
  показывает платформу, но строки/блоки таких тел недоступны без
  win32/macOS-скана. Полнота достигается многоконфигурационным прогоном
  (win32 — через mingw; macOS нужен osxcross/SDK).
- Канонические ключи зависят от `c++filt` (деманглинг Itanium ABI). На
  платформах без него C++-методы не получат namespace-квалификацию из
  mangledName (fallback — индекс по (имя, сигнатура)).
- `scan_uncovered` покрывает только файлы `src/` (по умолчанию); тестовые
  домены сканируются отдельно и в coverage_pct не входят.
- Многоконфигурационный merge опирается на тело (file+l0+l1) как
  идентичность: две конфигурации с одним и тем же диапазоном, но разными
  телами (переключение внутри функции через #if) будут считаться одним
  телом — редкий крайний случай, документируется в жизненном цикле.

## Uncovered-острова (`scan_uncovered.py`)

`coverage_pct` — доля «кодовых» строк src/, представленных хотя бы в одном
AST-узле хотя бы одного конфига. Оставшиеся острова — это:
- **platform**: ветки apple/bsd/solaris и прочие платформы без локального SDK;
- **option**: выключенные опции (ENABLE_MEMCHECK, __SANITIZE_THREAD__ и т.п.);
- **none/unknown**: код вне регионов или в неклассифицированных условиях —
  кандидаты на ручной разбор («остатки смысла»).

Для каждого острова артефакт хранит `{file, l0, l1, region, cls, cond,
lines}` — по ним можно решить: это «живой» код (нужен ещё один конфиг/
опция) или мёртвый (кандидат на удаление/рефакторинг).

## Открытые вопросы

- ~~Сканер: лёгкий regex-парсер vs clang-инструментация~~ → **clang**:
  ast-dump=json (блоки/рёбра) + `-E -dD` (макросы). Обход системных
  хидеров — фильтром по `loc.file`/tracker; `file` эмитится clang-ом только
  при смене FileID (дельта-кодирование), поэтому нужен сквозной трекер.
- Статус миграции: отдельная таблица `migration_map` или поле `cpp_status`
  в symbols (пока поле).
- `groups` наполнение: автогенерация из coverage/ci/задач + курируемо.
- Ветки `#else` получают буквальную инверсию условия (`!A && !B`) — для
  сложных условий полезен нормализатор/мини-оценщик в cond_regions.