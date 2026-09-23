# memory-hygiene — гигиена MCP-memory (Level 2, для всех ролей)

> Источник: `skynet/memory-codex.md`. Общий принцип:
> **MCP-memory — шпаргалка, документация в репозитории — источник истины.**

## Правила

1. **Memory хранит указатели, не содержание.** ADR/регламенты/архитектура —
   в origin-tree (`skynet/*.md`), в памяти — ссылка.
2. **Что хранить:** current milestone, активные sprint goals, WIP-статус,
   recent decisions (<5 спринтов), risks, Kaizen-бэклог, velocity-метрики,
   module-lock map, OBSERVATIONS pending.
3. **Что НЕ хранить:** содержимое ADR/регламентов, полный текст review,
   исходный код, context-packages, завершённые Kaizen.
4. **Единственный писатель памяти — `memory-mdbx-server` (MCP).** Скрипты/агенты
   пишут наблюдения через MCP-вызовы, не правят `graph.mdbx`/legacy `memory.jsonl`
   напрямую (канон и ловушки — `memory-codex.md`).
5. **Гигиена:** раз в 3 спринта очистка; устаревшие записи архивируются
   в документацию. Правило: **нет в memory — нет в проекте** (незафиксированное
   решение не принято).

## Knowledge-harvest (сбор знаний)

После крупной работы агент фиксирует «уроки»: инварианты, ловушки именования,
связи S-подсистем (см. `skills/shared/libmdbx-invariants/SKILL.md`) — как наблюдение в свою entity
или как KAIZEN/предложение по обновлению role-SKILL. Эти записи входят в
Weekly Codex Review (Level 4).