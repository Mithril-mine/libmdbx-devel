# SKILL: Scrum Ceremonies (Level 4, оркестратор)

> Режимы оркестратора: координатор (Planning), менеджер (Sync/Review),
> аналитик (Retrospective/Refinement). Один режим за раз — по церемонии.

## Спринты

- Длина 1–3 дня (по объёму Sprint Goal на Planning).
- **Sprint Goal обязателен** — без него спринт не стартует.
- Sprint cancellation: человек или оркестратор с согласия человека → новый Planning.

## Церемонии («по решению оркестратора, обязательно если — не реже чем»)

| Церемония | Обязательно если | Не реже чем | Режим |
|---|---|---|---|
| Sprint Planning | старт спринта | каждый спринт | координатор |
| Daily Sync | blocked-таски, конфликты module-lock | раз в день | менеджер |
| Sprint Review | конец спринта | каждый спринт | менеджер |
| Retrospective | rework >30%, cycle time растёт | раз в 2 спринта | аналитик |
| Backlog Refinement | >10 unprocessed, >5 OBSERVATIONS | раз в 3 спринта | координатор+аналитик |
| **Weekly Codex Review** | накопление опыта/знаний | раз в неделю | аналитик |

Форма минимальная: Daily Sync — проходка по ACTION-NEEDED + письма; Retrospective —
структурированный самоанализ по чек-листу (self-monitoring.md).

## Velocity-метрики (в registry)

Число завершённых тасков, средний cycle time, rework rate, compile/тест-проходимость.
Метрики — материал для Planning/Retrospective, не самоцель.

## Weekly Codex Review

Ревью всех `skynet/skills/**` Task-Protocols + L0 (`functional-architecture.md`,
`build.md`, `test-scenarios-codex.md`, `module-interfaces.md`, `probes.md`) по
накопленному опыту (OBSERVATIONS, журнал улучшений, knowledge-harvest из memory-hygiene.md).
Результат: обновления кодекса (коммит в devel, только документация) + метрика
«обновлено/устарело protocol-ов». BACKLOG: B34.

## Жизненный цикл спринта

Planning → Daily Sync → разработка → Review → Retrospective → Refinement.
Чек-лист Phase 0: инвентаризация тестов, smoke-CI-гейт, characterisation,
coverage baseline, декомпозиция epics, Sprint Goal, активная тройка, module-locks.