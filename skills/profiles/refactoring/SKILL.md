---
name: refactoring
description: Profile for deep refactoring of libmdbx (B+tree/CoW/GC/API) under characterisation-test protection, honoring subsystem conflict matrix and invariants.
---

# Refactoring profile — глубокий рефакторинг libmdbx

> Обязательный контекст: [`docs/engineering/libmdbx-invariants.md`](../../../docs/engineering/libmdbx-invariants.md),
> [`docs/engineering/test-durability-strategy.md`](../../../docs/engineering/test-durability-strategy.md),
> [`docs/engineering/module-lock-matrix.md`](../../../docs/engineering/module-lock-matrix.md).

## Область

Рефакторинг B+tree/CoW/GC/API под защитой characterisation-тестов. Знает, какие
подсистемы трогать параллельно, а какие последовательно (module-lock matrix).
Edge cases GC, meta-тройки, cursor tracking.

## Task Protocols

### Рефакторинг B+tree / CoW (REFACTOR-CORE)
MODULE-LOCK + characterisation → определить инварианты → рефакторить (CoW frozen
не модифицируется; repoint при CoW; path-to-root сохранён) → characterisation PASS
→ unit (safe_nosync, /dev/shm) PASS → amalgamate.py PASS → отчёт.

### Рефакторинг GC (REFACTOR-GC)
MODULE-LOCK S4 → понять GC-логику (дерево ключ=txnid знач=PNL; детент; LIFO/FIFO;
bigfoot; rp_augment_limit/gc_time_limit) → рефакторить (инвариант детента,
достижимости; LIFO/FIFO не менять без ADR) → characterisation → stress:
долгий читатель+запись, bigfoot-цепочка, исчерпание GC → MAP_FULL/рост корректен
→ amalgamate.py PASS.

### C→C++ migration (MIGRATE-C-TO-CPP)
MODULE-LOCK S1 → ABI test suite PASS, signal safety, TLS/setjmp →
мигрировать → ABI/signal/characterisation/amalgamation PASS → ADR решения.

## Output Format

```
REFACTOR-RESULT: task-id, status, files-changed, lines-changed,
  invariants-affected, tests-passed, characterisation-tests, stress-tests,
  amalgamation, abi-tests, signal-safety, observations
```

## Anti-Patterns

- GC без stress — race conditions.
- Без characterisation — курица и яйцо.
- LIFO/FIFO без ADR.
- C→C++ без ABI test suite.
- Игнор amalgamation при структурных изменениях.
- Разыменование указателей после парковки — use-after-free.