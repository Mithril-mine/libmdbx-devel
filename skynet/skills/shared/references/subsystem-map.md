# Subsystem Map — карта подсистем с уровнями риска

> Reference (Level 2, `shared/references/`). Часть иерархии SKILLS, см.
> `skynet/skills/README.md`. Исходник: `functional-architecture.md`,
> `structure.md` (§2/§7), `module-interfaces.md`, `skills/shared/libmdbx-invariants/SKILL.md`.
> Назначение: единая карта S1–S14 для оценки рисков при рефакторинге,
> планировании тестов и доменных ревью.

## 1. Матрица подсистем и рисков

| ID | Подсистема | Ключевые модули (`src/`) | Риск рефакторинга | Уровень |
|----|-----------|--------------------------|-------------------|---------|
| S1 | API-поверхность (C/C++) | `api-*.c`, `mdbx.c++`, `mdbx++/` | ABI-совместимость, сигнатуры, аллокации на границе | 🔴 высокий |
| S2 | B+tree | `node.c`, `cursor.c`, `dpl.c`, `page-*.c`, `tree-*.c`, `walk.c`, `sort.c`, `comparators.*` | CoW-инвариант, cursor tracking, split/merge | 🔴 высокий |
| S3 | CoW / жизненный цикл страниц | `page-ops.h`, `dml.c`, `txl.c`, `spill.c`, `dirtylist` | frozen/spilled/shadowed/modified, touch, dirty list | 🔴 высокий |
| S4 | GC | `gc-get.c`, `gc-put.c`, `rkl.c` | минная зона: детент, RLT, bigfoot, LIFO/FIFO | 🔴 высокий |
| S5 | Meta-тройка / геометрия | `meta.c`, `meta.h`, `dxb.c` | двухфазное обновление, 216 состояний, crash recovery | 🔴 высокий |
| S6 | Транзакции / MVCC | `txn.c`, `txn-basal.c`, `txn-nested.c`, `txn-ro.c`, `mvcc-readers.c`, `rthc.c` | commit pipeline (7 шагов), снапшот, park/unpark | 🔴 высокий |
| S7 | Большие значения / overflow | `page-ops.h` (overflow), `dml.c`, `bigfoot` | контигуальность, churn, S4+S2 зависимости | 🟠 средний |
| S8 | Долговечность / синхронизация | `dxb.c`, `osal.h`, `coherency.c` | DURABLE/NOMETASYNC/SAFE_NOSYNC/UTTERLY_NOSYNC/WRITEMAP | 🟠 средний |
| S9 | Параллелизм / координация | `lck.c`, `lck-posix.c`, `lck-windows.c`, `rthc.c`, `txl.c` | RLT, HSR, файл блокировок, TLS, fork | 🔴 высокий |
| S10 | Рост и переполнение | `dxb.c`, `gc-put.c`, `global.c` | MAP_FULL, auto-compaction, непереиспользуемые страницы | 🟠 средний |
| S11 | Курсоры / трекинг | `cursor.c`, `cursor.h`, `api-cursor.c` | repoint, lazy repositioning, shadow/couple, dangling | 🔴 высокий |
| S12 | Сервисные механизмы | `chk.c`, `defrag.c`, `backup`, `histogram.c`, `api-extra.c` | читающие пути, online | 🟡 низкий |
| S13 | Get-cached (ленивый кэш) | `api-get-cached.c` | 7 статусов: HIT/CONFIRMED/REFRESHED/DIRTY/BEHIND/UNABLE/RACE | 🟠 средний |
| S14 | WAF / оптимизация записи | `dml.c`, `page-*.c`, `prefer_waf_insteadof_balance` | path-to-root, батчинг, баланс vs WAF | 🟠 средний |

## 2. Инварианты подсистем (чек-лист)

Полный список — `skills/shared/libmdbx-invariants/SKILL.md`. Ключевое:

1. **Диск-формат заморожен** — `MDBX_DATA_VERSION` неизменен; структурные правки запрещены.
2. **Читатели wait-free** — никакие изменения не должны добавлять блокировок на путь чтения.
3. **TLS-destructor контракт** (`rthc_thread_dtor`) — чистка reader-записей при выходе потока.
4. **Page state machine** — `frozen/spilled/shadowed/modifiable/tmp` через сравнение txnid.
5. **GC не выдаёт страницы защищённые активными читателями** — `gc_reclaiming_obstacle`, `mvcc_kick_laggards`.
6. **Курсорная 4-состоятельная машина** — `poor/hollow/pointed/filled`, `z_eof_soft`/`z_eof_hard`.

## 3. Как пользоваться картой

- **Планирование рефакторинга**: риск 🔴 требует TDD + characterisation-тесты до изменения;
  🟠 — минимум P1-ut и смежные домены; 🟡 — обычный flow.
- **Распределение задач**: одна подсистема = один agent-владелец (module-lock), конфликты —
  `module-lock-matrix.md`.
- **Доменные ревью**: `REV:<domain>` по подсистемам (см. `skills-roles.md`).

> Синхронизировать с `structure.md`/`functional-architecture.md` при изменении состава
> `src/` модулей (Weekly Codex Review).