# SKILL: Kaizen Engine (Level 4, оркестратор)

> Механизм улучшения процессов. Принципы — в `skills/shared/kaizen-principles/SKILL.md`
> (L2); здесь — операционный механизм оркестратора.

## Анализ процессов (не кода)

Анализируются: процедуры управления, регламенты, коммуникация (человек+рой),
CI/CD, тестирование, ревью/рефакторинг.

## Входные данные

- OBSERVATIONS из отчётов агентов (накопитель: registry / `.skynet/tmp/<role>/observations/`).
- Velocity-метрики, паттерны блокеров (>3 повторов → сигнал), ретроспективы,
  внешние сигналы человека.

## Механизм

1. На Refinement/Weekly Review собрать наблюдения.
2. Сформировать **одно** предложение за ретроспективу: KAIZEN-PROPOSAL
   { PROBLEM, EVIDENCE, PROPOSAL, METRIC-BEFORE, METRIC-TARGET, REVERSIBILITY,
   IMPLEMENTATION, STATUS }.
3. Обратимость обязательна (шаги отката).
4. Внедрение = отдельный таск/epic со стандартным циклом.
5. STATUS: PROPOSED→APPROVED→IMPLEMENTING→VERIFIED|REJECTED|ROLLED-BACK.

## OBSERVATIONS-pipeline

- Агент пишет `observations:` в каждом TEST/REVIEW/REFACTOR/FEATURE/BUILD/DOC-RESULT.
- Накопитель обрабатывается на Refinement (>5 pending → обязателен) и Weekly Review.
- Итог: → задачи/epics (в `skynet_tasks`) или → обновление role-SKILL/Task-Protocols,
  или отклонение с причиной.