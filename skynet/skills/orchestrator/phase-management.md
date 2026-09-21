# SKILL: Phase Management (Level 4, оркестратор)

> Баланс рефактор/девелоп 80/20 → 20/80; сдвиг на Sprint Planning.
> Критерий перехода: rework < 20% и cycle time стабилен 3 спринта; решение — человек.

## Фазы

| Период | Рефактор/Девелоп | Активная тройка (пример) |
|---|---|---|
| Phase 0: Test infra | 100/0 | tests_writer + review-cmake + tests_worker |
| Phase 0: Coverage | 80/20 | tests_worker + reviewer + docs |
| Phase 1: Decomposition | 90/10 | tests_worker ×2 + docs |
| Phase 1.5: ABI gate | — | ABI test suite обязателен перед Phase 2 |
| Phase 2: Deep review | 80/20 | reviewer ×2 + tests_worker |
| Phase 3: Deep refactor | 70/30 | tests_worker + docs + reviewer |
| Development | 30/70 | tests_writer + tests_worker + reviewer |
| Mature | 20/80 | feature dev ×2 + тест-предохранитель |

## Правила по фазам

- Phase 0–1: Linux+Windows primary gates; macOS с Phase 1; Android best-effort.
- Phase 2 (deep review): три угла — C-core (память/UB/thread-safety), C++
  (RAII/exceptions), Architecture (coupling, ABI, #ifdef-леса, циклы). Debt-items
  в бэклог: critical→high→medium→low.
- Phase 3 (deep refactor): под защитой characterisation; рискованные модули первыми;
  после каждого рефакторинга — перезапуск characterisation; падение = баг или
  intentional change (документировать).
- Phase 1.5: ABI compatibility test suite (struct layouts, signatures, on-disk,
  signal safety) — обязательный milestone перед C→C++ migration Phase 2.
- Phase 4+: фичи; review на каждый merge; тесты на каждую фичу; рефакторинг из
  debt-бэклога при critical/blocking.