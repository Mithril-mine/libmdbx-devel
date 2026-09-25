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
| `call_edges` | DUPSORT | `caller_id` → set `callee_id` (+ `kind=syntax\|semantic`) |
| `groups` | DUPSORT | `group:{kind}:{name}` → set member_key |
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
| `scan_symbols.py` | symbols, call_edges(syntax) | `src/` (парсер) |
| `collect_coverage.py` | coverage | ctest/CI-артефакты |
| `collect_test_metrics.py` | test_metrics | ctest XML / LastTest.log / CI |
| `map_ci_jobs.py` | ci_jobs | `.github/workflows/*.yml` |
| `refresh_stale.py` | пометка `stale` | diff между генерациями |

Паттерн исполнения — как `tools/build_libmdbx.py`; детерминированный прогон,
сверка счётчиков с эталонным подмножеством (валидатор генератора).

## Дорожная карта

1. **Шаг 0 (готово)**: фиксация контекста (этот документ, память, BACKLOG B65).
2. **Шаг 1 (пилот — реализован)**: `scan_symbols.py` (C + C++, функции/типы/
   макросы/блоки/рёбра, clang ast-dump=json + `-E -dD` для макросов),
   валидатор `validate_map.py` (сверка с golden-счётчиками), таблицы
   symbols/call_edges/groups + батчевый загрузчик `load_map.py`.
   Текущие счётчики артефакта (Linux x86_64, clang 18):
   functions 2941, types 368, macros 550, blocks 29772, edges 17785
   (unresolved 5124 — системные вызовы/`__builtin_*`).
3. **Шаг 2**: semantic-рёбра, реестр probes (из `skynet/probes.md`), коллекторы
   coverage/test_metrics/ci_jobs, `refresh_stale`.

## Открытые вопросы

- ~~Сканер: лёгкий regex-парсер vs clang-инструментация~~ → **clang**:
  ast-dump=json (блоки/рёбра) + `-E -dD` (макросы). Обход системных
  хидеров — фильтром по `loc.file`/tracker; `file` эмитится clang-ом только
  при смене FileID (дельта-кодирование), поэтому нужен сквозной трекер.
- Статус миграции: отдельная таблица `migration_map` или поле `cpp_status`
  в symbols (пока поле).
- `groups` наполнение: автогенерация из coverage/ci/задач + курируемо.