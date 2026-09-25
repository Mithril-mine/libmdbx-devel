# SCHEMA — структура хранилища MCP-shared-graph-memory

> Схема DBI, формирование ключей, форматы записей и API.
> Эволюционировала из исходной спецификации владельца; целочисленные индексы
> — по `docs/DESIGN.md`.

## 1. Таблицы (DBI) — 11

| DBI | Флаги | Ключ → Значение | Назначение |
|---|---|---|---|
| `records` | DEFAULTS | канонический ключ → JSON body | основные записи |
| `vocab` | DEFAULTS | `module:{m}` / `{m}:{topic}` → `{}` | контролируемый словарь |
| `history` | DEFAULTS | `_history:{key}:{ts}` → JSON | версии записей |
| `ids` | DEFAULTS | record_key → uint64 id | key → id |
| `id2key` | INTEGERKEY | uint64 id → record_key | id → key |
| `inverted` | DUPSORT\|DUPFIXED\|INTEGERDUP | term → set of uint64 id | постинг-списки |
| `links` | DUPSORT\|DUPFIXED\|INTEGERDUP | `subject_id\0predicate` → set of object_id | граф связей |
| `simhash` | INTEGERKEY | uint64 id → uint64 hash | дедупликация (Hamming ≤ 3) |
| `access` | INTEGERKEY | uint64 id → 24 B: score+last_access+count | LRU/тиринг |
| `archive` | DEFAULTS | record_key → JSON | миграция cold-записей |
| `meta` | DEFAULTS | `next_id` → uint64 | sequence id |

`mdbx_env_set_maxdbs(env, 32)`. Одна write-транзакция на операцию.

### Ключевые решения (обоснование — в `DESIGN.md`)

- **dupsort живёт только в двух таблицах** (`inverted`, `links`) и только как
  постинг-набор целых id `uint64` — это снимает лимит ~2022 B на значение
  (корень инцидента B48).
- `id2key`/`simhash`/`access` — `INTEGERKEY` (быстрые компактные числовые ключи).
- Прикладной лимит значения записи — 16 MiB (`Store(max_value_bytes=...)`).

## 2. Формирование ключей

### 2.1 Структура

```
{тип}:{модуль}:{тема}
```

- **тип** — `decision` \| `bug` \| `proc` \| `bottleneck` \| `fact` \| `event`;
- **модуль** — из контролируемого словаря (базовые: `crypto`, `network`,
  `storage`, `ui`, `build`, `testing`, `review`, `platform`, `core`, `memory`);
- **тема** — 1–3 нормализованных слова через дефис, из словаря, ≤ 30 символов.

### 2.2 Нормализация (детерминированная)

1. lowercase + NFKC;
2. kebab-case (слова через дефис);
3. без артиклей/предлогов/союзов (en+ru стоп-слова);
4. без версий платформы и дат в теме (токены с цифрами отбрасываются);
5. максимум 3 слова, длина темы ≤ 30 символов;
6. проверка по словарю: модуль и тема должны существовать в `vocab`;
7. если нет — ошибка `invalid`, агент должен вызвать `vocab_add`.

### 2.3 Anti-patterns (запрещено)

- Свободный текст в ключе.
- Синонимы в одном словаре («testing» и «tests» одновременно).
- Изменение арности (2 или 4 сегмента вместо 3).
- Вложенные двоеточия в теме.
- Дата или версия в ключе.

## 3. Формат записи (JSON body в `records`)

```json
{
  "type": "bug",
  "summary": "1–3 предложения, без воды",
  "importance": 0.7,
  "date": "2026-09-25",
  "_id": 42,
  "_terms": ["segfault", "arm64"]
}
```

`summary`, `importance`, `date` — пользовательские; `_id`, `_terms` — служебные
(индексация, обновление inverted). `Связи` в теле НЕ хранятся — управляются
`link`, в человекочитаемых выводах строятся из `graph`.

## 4. API процедур (MCP tools)

Полный список и сигнатуры — в `SKILL.md`. Кратко:

| Процедура | Поведение |
|---|---|
| `safe_store` | normalize → merge (старая версия → `history`) или SimHash-дедуп (`conflict`) → создание + индексация (ids/id2key/inverted/simhash/access) в одной write-txn |
| `recall(pattern, limit≤10)` | prefix-скан `records` + композитный score |
| `search(query)` | термы → пересечение постинг-списков `inverted` → score |
| `lookup(term)` | точный поиск по `inverted` → ключи |
| `link` / `unlink` | Рёбра `{sid}\0pred` → oid и обратное `{oid}\0back:pred` → sid |
| `graph(key, depth≤3)` | BFS по связям, дедуп рёбер |
| `vocab_add` / `vocab_find` | пополнение/поиск словаря (fuzzy) |
| `dump_context` / `restore_context` | снимок `proc:memory:context-snapshot` |
| `gc(dry_run, archive)` | тиринг hot/warm/cold; archive перемещает cold в `archive` |
| `purge(keys)` | явное удаление записи + всех её индексов и связей |
| `stats()` | количество/типы/средняя важность/top-linked/словарь |

## 5. CLI-утилиты (`python -m mcp_memory.cli`)

```
dump   [--type T] [--module M] [--key K]
stats
graph  --key K [--depth N] [--format json|dot]
audit
gc     [--execute] [--archive] [--threshold F]
vocab  [--add module|topic --value V] [--find Q]
purge  KEY [KEY ...]
```

## 6. Жизненный цикл записи

```
Создание:   safe_store → normalize → vocab check → SimHash-дедуп →
            records + ids/id2key + inverted(terms→id) + simhash + access  [1 txn]
Обновление: safe_store → old → history; body → records; terms diff → inverted
Конфликт:   близкий SimHash (Hamming ≤ 3) → "conflict: {key}" → агент решает
Удаление:   purge → records/ids/id2key/inverted/simhash/access + связи → чистка
Архив:      gc(archive=true) → cold-записи в `archive`, из `records` удаляются
```

## 7. Лимиты (измерены на 4K-странице)

| Сущность | Лимит |
|---|---|
| Ключ | ≈ ½ страницы (~2022 B @4K) — для наших ключей недостижимо |
| Значение в dupsort-таблице | ≈ ½ страницы — обходится INTEGERDUP-постингами |
| Значение в non-dupsort | до ~2 GiB; прикладной лимит Store — 16 MiB |
| Именованные таблицы | ≤ `MDBX_MAX_DBI` (32765) |