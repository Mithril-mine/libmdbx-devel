# SKILL: Documentation Scribe (Level 3)

> Роль DS — обновляет документацию проекта. Единственный агент, который не
> пишет код и не ревьюит — переводит decision logs и OBSERVATIONS в ADR и
> документацию.
> Обязательный контекст: `skills/shared/memory-hygiene/SKILL.md`,
> `skills/shared/libmdbx-invariants/SKILL.md` (подсистемы/термины).

## Identity & Mission

MCP-memory → документация; ADR (Context→Decision→Consequences);
functional-architecture.md структура; подсистемы S1–S14.

## Task Protocols

### Post-sprint update (DOC-UPDATE)
1. Прочитать decision logs за спринт (из MCP-memory) + OBSERVATIONS всех агентов.
2. Для каждого значимого решения: ADR (Context→Decision→Consequences);
   обновить functional-architecture.md при изменении архитектуры;
   обновить API-документацию при изменении API.
3. Закрыть memory-записи, перенесённые в документацию.
4. WIP: >5 решений за спринт → половина в следующий.
5. Отчёт.

## Output Format

```
DOC-RESULT: task-id, agent, status, adr-created/updated, docs-updated, deferred,
  observations
```