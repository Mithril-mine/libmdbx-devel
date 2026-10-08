# Инварианты libmdbx — сводный чек-лист (S1–S14, ABI, модульные риски)

> Сводный справочник критических инвариантов и границ подсистем libmdbx.
> Источники: [`functional-architecture.md`](functional-architecture.md) (механизмы, §2.3/§2.4/§5.2/§6.2),
> [`structure.md`](structure.md) (карта модулей), [`module-interfaces.md`](module-interfaces.md).
> Используется как чек-лист при ревью, написании тестов и рефакторинге;
> детальная матрица конфликтов подсистем — [`module-lock-matrix.md`](module-lock-matrix.md).

## 1. Базовые свойства

libmdbx — встраиваемая транзакционная key-value БД, потомок LMDB. Ключевые свойства:

- **B+tree** — сбалансированные страницы-деревья; поиск спуском корень→лист.
- **MVCC** — страница помечена `txnid` создания; читатели видят снапшот без блокировок.
- **CoW (copy-on-write)** — запись создаёт новую версию страницы, видимая не меняется.
- **Memory-mapped файл** — чтение = обращение к mmap; нет буферного кэша.
- **Без WAL** — атомарность через двухфазное обновление тройки мета-страниц.
- **Wait-free читатели, один писатель** — чтение без блокировок, запись сериализована.
- **Встроенный GC** — освобождённые страницы переиспользуются, когда не видны читателям (детент).

## 2. Подсистемы и границы

| ID | Подсистема | Зона ответственности | Риск при рефакторинге |
|----|-----------|---------------------|----------------------|
| S1 | API-поверхность (C/C++) | env/txn/dbi/cursor/value | ABI-совместимость, сигнатуры |
| S2 | B+tree | поиск/вставка/удаление, split/merge, мультизначения | CoW-инвариант, cursor tracking |
| S3 | CoW / жизненный цикл страниц | frozen/spilled/shadowed/modified, touch, dirty list | все, кто работает с mmap-страницами |
| S4 | GC | GC-дерево, детент, LIFO/FIFO, bigfoot, rkl, rp_augment_limit, gc_time_limit | минная зона: RLT, детент, писатель |
| S5 | Meta-тройка / геометрия | двухфазное обновление, 216 состояний, геометрия | критично: crash recovery |
| S6 | Транзакции / MVCC | снапшот, RLT, commit pipeline (7 шагов), nested, park/unpark | взаимодействие GC/meta/cursor |
| S7 | Большие значения / overflow | overflow-страницы, контигуальность, churn, bigfoot | S4 + S2 |
| S8 | Долговечность / синхронизация | DURABLE/NOMETASYNC/SAFE_NOSYNC/UTTERLY_NOSYNC/WRITEMAP | crash recovery, perf |
| S9 | Параллелизм / координация | RLT, HSR, файл блокировок, TLS, fork, парковка/вытеснение | межпроцессные примитивы |
| S10 | Рост и переполнение | непереиспользуемые страницы, MAP_FULL, auto-compaction | S4/S5/S8 |
| S11 | Курсоры / трекинг | signature-based repoint, lazy repositioning, shadow/couple, dangling | CoW-зависимость |
| S12 | Сервисные механизмы | backup, chk, defrag, статистика | читающие пути, online |
| S13 | Get-cached (ленивый кэш) | 7 статусов: HIT/CONFIRMED/REFRESHED/DIRTY/BEHIND/UNABLE/RACE | ABA, race |
| S14 | WAF / оптимизация записи | path-to-root, батчинг, merge/rebalance, prefer_waf_insteadof_balance | баланс vs WAF |

Полная карта подсистем с ключевыми модулями `src/*` и уровнями риска —
[`subsystem-map.md`](subsystem-map.md). Полная карта с терминами —
`functional-architecture.md` (разделы).

## 3. Критические инварианты (чек-лист в каждом ревью/тесте)

1. **Достижимость**: каждая не-мета страница до `first_unallocated` достижима ровно
   один раз — принадлежит дереву или перечислена в GC (functional-architecture §2.4).
2. **Детент**: страницы из GC-записи с ключом ≤ детента переиспользуемы; выше — заморожены (§6.2).
3. **CoW**: замороженная страница не модифицируется; создаётся новая с `front txnid` (§5.2).
4. **Мета-тройка**: двухфазное обновление; 216 состояний; читатель видит цельную мету (§2.3).
5. **Курсор**: после CoW стек курсора repoint'ится или помечается для lazy repositioning.
6. **WAF**: страница попадает в грязный список один раз за транзакцию (§5.3).

Дополнительные инварианты подсистем (диск-формат, wait-free читатели, TLS-destructor,
page state machine, GC/читатели, курсорная машина) — в [`subsystem-map.md`](subsystem-map.md) §2.

## 4. Граница C/C++ и ABI

- **C API** (`mdbx.h`): публичный интерфейс, ABI-stable; on-disk формат не зависит от платформы.
- **C++ API** (`mdbx.h++`): тонкий типобезопасный слой поверх C-ядра.
- **C++ migration**: разворот зависимости; требует ABI test suite + signal safety.
- **Amalgamation**: `amalgamate.py` собирает всё в один файл; структурные изменения
  `#include` обязаны пройти amalgamation-gate **до merge**.

## 5. Модульные риски (matrix HIGH/MED/LOW)

HIGH = последовательная работа (module-lock конфликт); MED = координация через
ограничения; LOW = параллельно безопасно. Пересечения по
S2/S3/S4/S6/S7/S11/S14 (B+tree↔CoW↔GC↔Txn↔Cursor↔Overflow↔WAF) —
полная матрица пар в [`module-lock-matrix.md`](module-lock-matrix.md).
В amalgamated сборке подсистемы в одном файле → module-lock ≈ file-lock.