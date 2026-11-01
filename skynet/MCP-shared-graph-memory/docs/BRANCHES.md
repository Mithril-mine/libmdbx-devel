# Слои веток: нелинейное версионирование базы знаний (BRANCHES)

> Решение: 2026-10 тред «пилот на GC + нелинейное версионирование».
> Связано: SCHEMA.md (§«слои веток»), LIFECYCLE.md, SKILL.md (раздел «Ветки»).

## Проблема

Одновременно существуют несколько веток кода: сильно различающиеся
(`stable`, `lts/0.13`) и близкие в процессе рефакторинга фич. Структурные
данные (карта исходников, покрытие, probes, traces) **различаются между
ветками**: одна функция имеет разные тела/блоки/рёбра в `master` и `stable`.
Без разделения слоёв перегенерация карты одной ветки затирала бы данные
другой — знания и «что на что влияет» смешивались.

## Модель

### Идентификатор слоя

```
{ver-slug}.{feature-slug}.{n}
```

- **`ver-slug`** — базовый НОМЕР ВЕРСИИ проекта (НЕ имя ветки!): `0.15`,
  `0.14`, `0.13`. Берётся из последнего тега `vX.Y.Z` (см. раздел
  «Версионирование имени»). Версия важнее имени ветки: master→stable→lts —
  номер версии стабилен.
- **`feature-slug`** — канонизированное имя feature-ветки (`gc-pilot-2`,
  `mcp-cortex`); для базовых веток — `base`.
- **`n`** — `git rev-list --count <merge-base>...feature`: монотонный
  счётчик коммитов от точки отделения. **Порядок/навигация, НЕ мера
  отличий**: масштаб различий даёт структурный diff содержимого
  (`cg_diff_branches`), а не счётчик коммитов. История может
  переписываться — slug+n устойчив к перезаписи (пока merge-base не
  меняется), поэтому hash НЕ входит в идентификатор.

Пример:
`0.15.gc-pilot-2.17` — 17-й коммит feature-ветки gc-pilot-2 от точки
отделения от версии 0.15.

### Базовые слои (без префикса)

Актуальный dev (`master`, сейчас версия 0.15) живёт **БЕЗ префикса** в
перегенерируемых таблицах (обратная совместимость). Регистрируется как
`branch:0.15` (или `0.15.base`). Существующие данные не трогаются.

### Версионирование имени (переходы веток)

Так как ветки «переезжают» по версиям, правило: **при смене версии точки
отсчёта** (например stable станет 0.14, master → 0.16) — базовый слой
перерегистрируется под новый номер, старый архивбруется:

| Событие | Действие |
| --- | --- |
| master становится stable | `branch_register("0.16","base")`; старый `0.15` → `status=archived` (слой остаётся, prefix переходит к 0.16) |
| stable становится lts/0.14 | слой 0.14 архивируется; lts-данные живут в `0.13.*` слоях |
| feature → merged | `cg_diff_branches(base, feature)` → перенос новых символов в base → пересканировать base → feature `status=merged` |

## Механизм схемы

- Таблица **`branches`** (DEFAULTS): `branch:{layer}` → JSON
  `{ver_slug, feature_slug, n_commits, revision, git_tag, base_branch, status, created_at, is_base}`.
- `meta.branch_current` — активный слой; зеркало в canary.z.
- **`cg_generations()`/canary.y — поколение ТОЛЬКО базового слоя** (N13):
  инкрементируется `cg_load_batch(replace=True)` лишь когда слой базовый
  (`pre==""`). У feature-слоя monotonic-поколения нет — `cg_snapshot(layer=feat)`
  возвращает `generation` базового canary, а не своего. Для контроля изменений
  feature-слоя используйте `cg_snapshot.content_hash` (считается по факту
  содержимого слоя) и `cg_diff_branches`, а не generation. Это by-design:
  generation задумано как «поколение основной карты», content_hash — как
  универсальный токен состояния любого слоя.
- **Курируемый слой** (`records/links/vocab/adr/test_metrics/ci_jobs/bridge`) —
  **общий на все ветки**: знания роя не ветвятся; ссылки на символы — по
  имени слоя в атрибуте связи (агент указывает layer в map-*).
- **Перегенерируемый слой** (всё, что описывает состояние кода ветки) — с
  префиксом `{layer}/` в ключе (базовый — без префикса): `symbols`,
  `call_edges(+rev)`, `groups`, `coverage`, `covered_by`, `coverage_blocks`,
  `probes`, `call_traces(+rev)`. Все MCP-методы этих семейств принимают
  `layer` (пустой → активная ветка); префиксация — единая точка в Store.
  **НЕ слоистые (общие на все ветки, пока не реализована layer-изоляция):**
  `regions`, `uncovered`, `project_files`, `file_deps` — `load_map.py`
  режет их загрузку для feature-слоя.
- **Префиксы исключительные**: базовый слой не видит чужих feature-ключей
  (регулярка `_LAYER_KEY_RE` отсекает «0.16.abc.N/…»).

## Инструменты

- MCP: `branch_register`, `branch_list`, `branch_current`, `branch_activate`
  (`expected_prev` — CAS от гонки в рое; активация НЕзарегистрированного
  слоя возвращает `warning` — чтения уйдут в пустой слой),
  `branch_remove` (реестр),
  `branch_purge` (слой С данными; базовый и АКТИВНЫЙ слой запрещены —
  сначала переключите ветку),
  `cg_diff_branches(from,to)`, `coverage_migrate(from,to,symbols?)`.
- MCP (запись карты, C2): `cg_put_symbol` (валидирует `cpp_status`),
  `cg_alias`, `cg_refresh_stale`.
- `layer=` принимают: `cg_symbol(s)`, `cg_edges_of`, `cg_callers_of`,
  `cg_put_edge`, `cg_put_symbol`, `coverage_insert/batch/remove/get/all`,
  `coverage_symbols`, `coverage_summary`, `coverage_gaps`,
  `coverage_blocks_insert/batch/get`, `probe_*`, `trace_*` (project — пока
  нет, см. «НЕ слоистые»).
- `load_map.py --layer <layer>` — загрузка карты в слой (базовый — без
  префикса).

## Процедуры

### Сканирование под ветку

```sh
# в worktree ветки: подготовить _build-scan, запустить сканер на ветку
python3 tools/scan_symbols.py --config linux \
    --out tools/artifacts/codegraph-${BRANCH}.json
python3 tools/load_map.py --map .../codegraph-${BRANCH}.json \
    --db <живая> --layer "0.15.${SLUG}.${N}"
```

### Слияние feature → base

1. `cg_diff_branches(base, feature)` — структурный diff содержимого:
   какие символы добавлены/изменены/удалены, по модулям, число блоков.
2. Пересканировать base (включить новые символы feature в base-слой).
3. Перенести `coverage/blocks/traces` в base-слой:
   `coverage_migrate(feature, base)` — копия (источник цел); при
   конфликте test_id/caller целевой слой перезаписывается свежими данными
   (merge-семантика; для coverage — набор сужается фильтром, для traces —
   caller копируется целиком телом источника). Фильтр `symbols=[...]` —
   только новые символы. Битые/несогласованные записи источника не
   роняют миграцию — уходят в отчёт `skipped`.
4. Feature-слой → `status=merged` (`branch_register(..., status="merged")`).
   Данные мёртвого слоя после rebase/слияния — `branch_purge(feature)`
   (базовый слой purge запрещён: он обновляется replace-загрузкой;
   активный слой тоже — сначала `branch_activate` на другой). После purge
   links curated-записей на его символы становятся висячими — прогоните
   `cg_refresh_stale`.

### Работа агента

- В feature-ветке: все инструменты, описывающие состояние кода
  (`map_*`, `coverage_*`, `probe_*`, `trace_*`) резолвят активную ветку
  или явный `layer`; **коллекторы** (`collect_coverage.py`,
  `gcov_to_coverage.py`, `load_map.py`) принимают `--layer`. Префиксация
  ключей — единая точка в Store, вызывающий не добавляет префикс вручную.
- **Активная ветка** (`branch_activate`) задаёт слой по умолчанию для всех
  этих инструментов (аргумент `layer` явно перебивает её). После активации
  проверяйте `branch_current`/`version_info` перед чтением.
- Знания (curated-слой: `records/links/vocab/adr/ci_jobs/test_metrics`) —
  общие, без layer.
- Перед «какие тесты покрывают X» (`coverage_symbols`) — указывать
  актуальный слой.
- `version_info` всегда показывает активный слой + ревизию.

## Возможные расширения (отложено)

- `0.16-master` → станет `0.16.base`: автоматическая миграция префикса.
- Ассоциация curated-ссылок со слоем символа (поле `_layer` в теле связи).
- `cg_diff_branches` — дополнение аргументом «только функции / только
  изменённые сигнатуры» (фильтры).
