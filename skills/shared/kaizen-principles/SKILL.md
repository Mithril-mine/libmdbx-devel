---
name: kaizen-principles
description: Principles of continuous improvement — one measurable, reversible improvement per retrospective; observations as the key signal source.
---

# kaizen-principles — принципы улучшения

> Это ПРИНЦИПЫ (общие), не механизм. Используй в ретроспективах и при выборе
> предложений по улучшению процессов.

## Принципы

1. **Одно улучшение за ретроспективу.** Не поток, а одно конкретное, измеримое,
   обратимое предложение.
2. **Измеримость.** Предложение содержит метрику «до» и целевую «после».
3. **Обратимость.** Предложение содержит шаги отката, если эффекта нет или хуже.
4. **Внедрение — отдельная задача** со стандартным циклом (не «на бегу»).
5. **OBSERVATIONS участников** — ключевой источник сигналов о процессах.

## Копилка подходов

Проверенные паттерны мышления (дивергенция, baseline-reset, асимметрия доступа,
dogfooding, декомпозиция потока) — [`references/approach-bank.md`](references/approach-bank.md)
(A1–A13). Использовать в brainstorming и при выборе решения.

## Источники данных

- Метрики скорости (cycle time, rework rate, проходимость).
- OBSERVATIONS из отчётов (`observations:` поле).
- Паттерны блокеров (что repeatedly мешает).
- Ретроспективные инсайты, внешние сигналы человека.

## Формат

```
KAIZEN-PROPOSAL { ID, PROBLEM, EVIDENCE, PROPOSAL, METRIC-BEFORE,
                  METRIC-TARGET, REVERSIBILITY, IMPLEMENTATION, STATUS }
```

STATUS: PROPOSED | APPROVED | IMPLEMENTING | VERIFIED | REJECTED | ROLLED-BACK.