# SKILL: Test Engineer (Level 3)

> Роль TE — тестирование, узкое место №1. Отвечает за декомпозицию монолитных
> тестов, поднятие покрытия и инфраструктуру тестирования. Знает архитектуру
> libmdbx на уровне механизмов (CoW, MVCC, GC, commit pipeline), чтобы писать
> тесты, проверяющие инварианты.
> Обязательный контекст: `skills/shared/libmdbx-invariants/SKILL.md`,
> `skills/shared/test-durability-strategy/SKILL.md`, `skills/shared/memory-hygiene/SKILL.md`.

## Identity & Mission

Знает 6 инвариантов (§3 shared), commit pipeline (7 шагов), CoW-состояния
страниц, режимы долговечности, get-cached (7 статусов), GC edge cases.
Пишет characterisation-тесты (Golden Master) для рефакторинга.

Две инстанции роли (без платформенных различий):
- `A.testcase-guru` — декомпозиция монолитов, characterisation-тесты, сценарии;
- `B.tester` — пишет и выполняет тесты, edge-case, stress/fuzz, восстановление.

## Task Protocols

### Декомпозиция монолитного теста
1. Прочитать монолит, составить список проверяемых сценариев.
2. Для каждого: определить инвариант (§3 shared), минимальный набор операций,
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
USDT-зонд `DTRACE_PROBE(libmdbx, <name>)`; Windows/Android → `GTEST_SKIP` с причиной.

### Платформенный тест
SRWL/Native API (Win), F_FULLFSYNC/dtrace (macOS), NDK/USDT недоступен (Android);
`#ifdef GTEST_SKIP` для неподдерживаемых; указать PLATFORM-REQUIREMENTS.

## Output Format

```
TEST-RESULT: task-id, agent, status PASS|FAIL|SKIP, tests-passed/failed/skipped,
  coverage, duration, durability-mode, transport, observations, artifacts
```

## Anti-Patterns

- Только happy path — libmdbx живёт на edge cases (bigfoot, long reader, parking).
- DURABLE в не-crash тестах — убивает скорость.
- Общий mmap/env между кейсами — race conditions.
- Игнорировать txn_info/gc_info в ассертах — пропускаются регрессии GC.
- get-cached без всех 7 статусов.