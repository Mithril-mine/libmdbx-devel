# kaizen-principles — принципы улучшения (Level 2, для всех ролей)

> Часть иерархии SKILLS. Это ПРИНЦИПЫ (общие), не механизм; механизм —
> в `skynet/skills/orchestrator/kaizen-engine/SKILL.md` (Level 4).

## Принципы

1. **Одно улучшение за ретроспективу.** Не поток, а одно конкретное, измеримое,
   обратимое предложение.
2. **Измеримость.** Предложение содержит метрику «до» и целевую «после».
3. **Обратимость.** Предложение содержит шаги отката, если эффекта нет или хуже.
4. **Внедрение — отдельный таск/epic** со стандартным циклом (не «на бегу»).
5. **OBSERVATIONS от агентов** — ключевой источник сигналов о процессах.

## Копилка подходов

Проверенные паттерны мышления (дивергенция, baseline-reset, асимметрия
доступа, dogfooding, декомпозиция потока) — `../references/approach-bank.md`
(A1–A12). Использовать в brainstorming и при выборе решения.

## Источники данных

- Velocity-метрики (cycle time, rework rate, проходимость).
- OBSERVATIONS из отчётов агентов (`observations:` поле).
- Паттерны блокеров (что repeatedly мешает).
- Ретроспективные инсайты, внешние сигналы человека.

## Формат

```
KAIZEN-PROPOSAL { ID, PROBLEM, EVIDENCE, PROPOSAL, METRIC-BEFORE,
                  METRIC-TARGET, REVERSIBILITY, IMPLEMENTATION, STATUS }
```

STATUS: PROPOSED | APPROVED | IMPLEMENTING | VERIFIED | REJECTED | ROLLED-BACK.