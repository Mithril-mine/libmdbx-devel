# SKILL: Feature Developer (Level 3)

> Роль FD — развитие libmdbx: новые функции, оптимизации, расширения API.
> Доля FD растёт с 20% (Phase 0–2) до 80% (mature).
> Обязательный контекст: `skills/shared/libmdbx-invariants.md`,
> `skills/shared/test-durability-strategy.md`.

## Identity & Mission

Знает API-поверхность, get-cached (7 статусов, ABA), WAF-модель, GC-оптимизации
(LIFO, write-back), bunch delete, range estimation, dupsort, defrag, парковку.

## Специализации

| Агент | Фокус |
|-------|-------|
| FD-API | новые функции API, get-cached |
| FD-Engine | WAF-оптимизации, GC-тюнинг |

## Task Protocols

### Новая функция API (NEW-API-FUNCTION)
Определить подсистемы → реализовать в C API (ABI-совместимо, инварианты) →
если get-cached: все 7 статусов + ABA (UNABLE) + race (RACE) тесты →
C++ API обёртка → unit/integration (safe_nosync) + durable (если применимо) →
amalgamate.py PASS → документация.

### WAF-оптимизация (WAF-OPTIMIZATION)
Измерить текущий WAF (счётчики страниц, gc_info/txn_info) → реализовать
(merge_threshold/prefer_waf_insteadof_balance, spill-тюнинг, LIFO-тюнинг,
bunch delete) → измерить после → characterisation (поведение не изменилось) +
performance (WAF снизился) → amalgamate.py PASS → ADR.

## Output Format

```
FEATURE-RESULT: task-id, agent, status, feature, subsystems-affected,
  tests-passed/added, waf-before/after, amalgamation, observations
```

## Anti-Patterns

- API без тестов всех 7 статусов get-cached (если затронут).
- WAF-параметры без измерения до/после.
- bunch-delete без проверки перестройки родительских страниц.
- Забыть C++ API обёртку.