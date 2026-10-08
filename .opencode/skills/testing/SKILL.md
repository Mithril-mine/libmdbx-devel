---
name: testing
description: Profile for testing libmdbx — monolithic test decomposition, characterisation (Golden Master) tests, CI pipeline setup, fault injection and platform-specific tests.
---

# Testing profile — тестирование libmdbx

> Обязательный контекст: [`docs/engineering/libmdbx-invariants.md`](../../../docs/engineering/libmdbx-invariants.md),
> [`docs/engineering/test-durability-strategy.md`](../../../docs/engineering/test-durability-strategy.md).
>
> Смежные материалы: [`docs/engineering/testing-methodology.md`](../../../docs/engineering/testing-methodology.md)
> (механизмы полноты покрытия COLLECT/WATCH/FAULT),
> [`docs/engineering/testing-codex.md`](../../../docs/engineering/testing-codex.md)
> (операционные правила C0–C6: тайеры, флаки, бюджеты),
> [`docs/engineering/test-scenarios.md`](../../../docs/engineering/test-scenarios.md)
> (ТЗ сценариев SC-*), [`docs/engineering/test-coverage.md`](../../../docs/engineering/test-coverage.md)
> (карта «тесты → модули», дыры), [`docs/engineering/probe-bus.md`](../../../docs/engineering/probe-bus.md)
> (движок инъекций mprobe v2), [`docs/engineering/probes.md`](../../../docs/engineering/probes.md)
> (каталог USDT/DTrace-маркеров), [`docs/engineering/testing-infra-design.md`](../../../docs/engineering/testing-infra-design.md)
> (дизайн CI v2), [`docs/engineering/debugging-methodology.md`](../../../docs/engineering/debugging-methodology.md)
> (инструменты отладки и сценарии).

## Область

Декомпозиция монолитных тестов, поднятие покрытия и инфраструктура тестирования.
Знает архитектуру libmdbx на уровне механизмов (CoW, MVCC, GC, commit pipeline),
чтобы писать тесты, проверяющие инварианты. Пишет characterisation-тесты
(Golden Master) для рефакторинга.

## Task Protocols

### Декомпозиция монолитного теста
1. Прочитать монолит, составить список проверяемых сценариев.
2. Для каждого: определить инвариант, минимальный набор операций,
   режим durability (SAFE_NOSYNC по умолчанию, DURABLE только для crash-recovery),
   написать изолированный GoogleTest-кейс, указать DURABILITY-MODE.
3. Запустить изолированно (каждый кейс самодостаточен).
4. Убедиться, что покрытие монолита сохранено (сравнить с исходным).
5. Удалить монолит после подтверждения покрытия.

### Characterisation test (Golden Master)
1. Зафиксировать текущее поведение модуля/функции (входные данные incl. edge,
   выходные, внутреннее состояние через env_info/txn_info/gc_info).
2. Золотой эталон → файл; тест сравнивает вывод и FAIL при расхождении.
3. НЕ утверждать правильность поведения — фиксируем как есть.
4. Режим: SAFE_NOSYNC + /dev/shm (или RAM-диск).

### Настройка CI-pipeline
По платформам (Linux/Win/macOS/Android): transport (shm/ramdisk/tmp), prerequisites
(systemtap-sdt-dev/dtrace/NDK), CTest-таймауты; amalgamation-gate ДО merge;
матрица Quick (safe_nosync, каждый push) / Full (durable, ночной); reporting.

### Fault injection (SystemTap/DTrace)
Точки инъекции по commit pipeline (GC-обработка/refund/spill/audit/meta update/sync);
USDT-зонд `DTRACE_PROBE(mdbx, <name>)`; Windows/Android → `GTEST_SKIP` с причиной.

### Платформенный тест
SRWL/Native API (Win), F_FULLFSYNC/dtrace (macOS), NDK/USDT недоступен (Android);
`#ifdef GTEST_SKIP` для неподдерживаемых; указать PLATFORM-REQUIREMENTS.

## Output Format

```
TEST-RESULT: task-id, status PASS|FAIL|SKIP, tests-passed/failed/skipped,
  coverage, duration, durability-mode, transport, observations, artifacts
```

## Anti-Patterns

- Только happy path — libmdbx живёт на edge cases (bigfoot, long reader, parking).
- DURABLE в не-crash тестах — убивает скорость.
- Общий mmap/env между кейсами — race conditions.
- Игнорировать txn_info/gc_info в ассертах — пропускаются регрессии GC.
- get-cached без всех 7 статусов.