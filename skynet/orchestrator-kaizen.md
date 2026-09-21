# SKILL: Оркестратор — координация роя (указатель, v2.12+)

> Полный SKILL разбит на 5 файлов Level 4 в `skynet/skills/orchestrator/`.
> Здесь — манифест и связи с протоколом. Режимы: координатор/менеджер/аналитик.

## Компоненты

| Файл | Содержание |
|---|---|
| `scrum-ceremonies.md` | спринты, церемонии (Planning/Sync/Review/Retrospective/Refinement/Weekly Codex Review), velocity |
| `swarm-management.md` | пул ролей, паттерны активации, nook-пул, module-locks, WIP |
| `kaizen-engine.md` | механизм анализа процессов, OBSERVATIONS-pipeline, KAIZEN-PROPOSAL |
| `phase-management.md` | Phase 0–4 + Phase 1.5 (ABI gate), баланс рефактор/девелоп |
| `self-monitoring.md` | антипаттерны, sequential-thinking, эскалация, rollback, self-check |

Принципы (общие, L2): `skills/shared/kaizen-principles/SKILL.md`,
`skills/shared/memory-hygiene/SKILL.md`, `skills/shared/libmdbx-invariants/SKILL.md`,
`skills/shared/test-durability-strategy/SKILL.md`. References:
`skills/shared/references/` (functional-architecture, subsystem-map, module-lock-matrix).

## Связь с протоколом (§15 в skynet-protocol.md)

- Пул busy ≤3 + оркестратор: §25a; канонические сессии: §25b; nook-пул: §25b (v2.13).
- Роли/ревью: §28, skills-roles.md; задачи: §14; claims §15; доска §19.
- Валидация §27; реестр §4/§5; конфликты §16; waitmail §23.