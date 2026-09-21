# SKILLS — иерархия (манифест)

> Организация навыков роя по 5 уровням. Адаптация владельческого набора
> (`agent_roles_skill.md`, `orchestrator_skill.md`) под текущий строй.
> Внешние Batch API / Vector Store не используются; их роль — доска
> `skynet_tasks`, письма, `.skynet/BACKLOG.md`, memory-codex.

## Уровни

| Level | Назначение | Файлы | Кто грузит |
|---|---|---|---|
| **L0** Project Reference | справочник проекта (не SKILL) | `functional-architecture.md` | по ссылке, НЕ целиком в контекст |
| **L1** Superpowers Core | 14 поведенческих навыков (общие) | `skynet/superpowers/` | все агенты |
| **L2** Shared Domain | проект-специфичные, общие для ролей | `skills/shared/` | все роли |
| **L3** Role-Specific | по одному на роль | `skills/roles/` | только нужная роль |
| **L4** Orchestrator | только оркестратор | `skills/orchestrator/` | main_architect |

## L2 Shared (4 файла)

- `libmdbx-invariants.md` — подсистемы S1–S14, 6 инвариантов, ABI, модульные риски.
- `test-durability-strategy.md` — режимы долговечности, транспорты, Quick/Full.
- `kaizen-principles.md` — принципы улучшения (одно за ретроспективу, измеримость, обратимость).
- `memory-hygiene.md` — MCP-memory как шпаргалка, документация как истина, knowledge-harvest.

## L3 Roles (6 файлов)

`test-engineer.md`, `code-reviewer.md`, `refactoring-engineer.md`,
`feature-developer.md`, `platform-build-engineer.md`, `documentation-scribe.md`.

Маппинг на агентов — в `skills-roles.md` (TE→tests_worker/tests_writer,
CR→review-cpp/win/macos/cmake, PB→review-cmake/win, DS→docs; RE/FD — gap-эталоны).

## L4 Orchestrator (5 файлов)

`scrum-ceremonies.md`, `swarm-management.md`, `kaizen-engine.md`,
`phase-management.md`, `self-monitoring.md`. Указатель — `orchestrator-kaizen.md`.

## Правила активации

- Агент читает свой role-SKILL (L3) + L1/L2 при бутстрапе/wake; L0 — по ссылке.
- Оркестратор — L1/L2 + L4.
- Weekly Codex Review (L4 scrum-ceremonies) раз в неделю ревьюит L2/L3/L4
  Task-Protocols и L0 по накопленному опыту.