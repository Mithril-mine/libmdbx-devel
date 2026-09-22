# SKILLS — иерархия (манифест)

> Организация навыков роя по 5 уровням. Адаптация владельческого набора
> (`agent_roles_skill.md`, `orchestrator_skill.md`) под текущий строй.
> Внешние Batch API / Vector Store не используются; их роль — доска
> `skynet_tasks`, письма, `.skynet/BACKLOG.md`, memory-codex.

## Уровни

| Level | Назначение | Каталог | Формат | Кто грузит |
|---|---|---|---|---|
| **L0** Project Reference | справочник проекта (не SKILL) | `shared/references/functional-architecture.md` | один файл | по ссылке, НЕ целиком в контекст |
| **L1** Superpowers Core | поведенческие навыки (общие) | `superpowers/` | `<name>/SKILL.md` | все агенты |
| **L2** Shared Domain | проект-специфичные, общие для ролей | `shared/` | `<name>/SKILL.md` | все роли |
| **L3** Role-Specific | по одному на роль | `roles/` | `<name>/SKILL.md` | только нужная роль |
| **L4** Orchestrator | только оркестратор | `orchestrator/` | `<name>/SKILL.md` | main_architect |

```
skills/
  superpowers/                    # Level 1 — содержимое как у obra/superpowers
    test-driven-development/SKILL.md
    systematic-debugging/SKILL.md
    ...
  shared/                         # Level 2
    libmdbx-invariants/SKILL.md
    test-durability-strategy/SKILL.md
    kaizen-principles/SKILL.md
    memory-hygiene/SKILL.md
    references/
      functional-architecture.md  # исходный файл (копия skynet/functional-architecture.md)
      subsystem-map.md            # карта подсистем S1–S14 с risk levels
      module-lock-matrix.md       # матрица конфликтов модулей
  roles/                          # Level 3
    test-engineer/SKILL.md
    code-reviewer/SKILL.md
    refactoring-engineer/SKILL.md
    feature-developer/SKILL.md
    platform-build-engineer/SKILL.md
    documentation-scribe/SKILL.md
  orchestrator/                   # Level 4
    scrum-ceremonies/SKILL.md
    swarm-management/SKILL.md
    kaizen-engine/SKILL.md
    phase-management/SKILL.md
    self-monitoring/SKILL.md
```

## L1 Superpowers Core (14 навыков, не трогаем)

Каталог `superpowers/` — адаптация obra/superpowers (MIT). Каждый навык — подкаталог
`<name>/SKILL.md`. Манифест: `superpowers/README.md`. Два навыка сознательно отложены
(defer, зафиксировано в `superpowers/README.md`): `subagent-driven-development`,
`dispatching-parallel-agents`.

## L2 Shared (4 SKILL + 3 references)

- `libmdbx-invariants/SKILL.md` — подсистемы S1–S14, 6 инвариантов, ABI, модульные риски.
- `test-durability-strategy/SKILL.md` — режимы долговечности, транспорты, Quick/Full.
- `kaizen-principles/SKILL.md` — принципы улучшения (одно за ретроспективу, измеримость, обратимость).
- `memory-hygiene/SKILL.md` — MCP-memory как шпаргалка, документация как истина, knowledge-harvest.
- `references/` — справочные материалы:
  - `functional-architecture.md` — L0 Project Reference (копия).
  - `subsystem-map.md` — карта подсистем S1–S14 с уровнями риска (🔴/🟠/🟡).
  - `module-lock-matrix.md` — матрица конфликтов модулей (HIGH/MED/LOW) для module-locks.

## L3 Roles (6 SKILL)

`test-engineer`, `code-reviewer`, `refactoring-engineer`, `feature-developer`,
`platform-build-engineer`, `documentation-scribe` — по одному каталогу `<name>/SKILL.md`.
Маппинг на агентов — в `skills-roles.md` (TE→A.testcase-guru/B.tester,
CR→D.reviewer-c/E.reviewer-cxx/F.reviewer-cmake, PB→C.ci-guru, DS→S.scribe;
RE/FD/DEV/RESEARCHER/TOOLING — свободные роли, gap-эталоны из L3 SKILLs).

## L4 Orchestrator (5 SKILL)

`scrum-ceremonies`, `swarm-management`, `kaizen-engine`, `phase-management`,
`self-monitoring` — по одному каталогу `<name>/SKILL.md`. Указатель —
`orchestrator-kaizen.md`.

## Правила активации

- Агент читает свой role-SKILL (L3) + L1/L2 при бутстрапе/wake; L0 — по ссылке.
- Оркестратор — L1/L2 + L4.
- Weekly Codex Review (L4 scrum-ceremonies) раз в неделю ревьюит L2/L3/L4
  Task-Protocols и L0 по накопленному опыту.