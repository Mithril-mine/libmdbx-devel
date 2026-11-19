# Инварианты libmdbx — сводный чек-лист (S1–S14, ABI, модульные риски)

> Сводный справочник критических инвариантов и границ подсистем libmdbx.
> Источники: [`deep-dive.ru.md`](../ru/deep-dive/internals.md) (механизмы, §2.3/§2.4/§5.2/§6.2),
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
`deep-dive.ru.md` (разделы).

## 3. Критические инварианты (чек-лист в каждом ревью/тесте)

1. **Достижимость**: каждая не-мета страница до `first_unallocated` достижима ровно
   один раз — принадлежит дереву или перечислена в GC (deep-dive.ru.md §2.4).
2. **Детент**: страницы из GC-записи с ключом ≤ детента переиспользуемы; выше — заморожены (§6.2).
3. **CoW**: замороженная страница не модифицируется; создаётся новая с `front txnid` (§5.2).
4. **Мета-тройка**: двухфазное обновление; 216 состояний; читатель видит цельную мету (§2.3).
5. **Курсор**: после CoW стек курсора repoint'ится или помечается для lazy repositioning.
6. **WAF**: страница попадает в грязный список один раз за транзакцию (§5.3).
7. **Детерминизм позиционирования**: неудача любой команды позиционирования
   оставляет курсор в единственном детерминированном состоянии, определённом
   для этой команды, — одинаковом для всех внутренних путей исполнения
   (§5).

Дополнительные инварианты подсистем (диск-формат, wait-free читатели, TLS-destructor,
page state machine, GC/читатели, курсорная машина) — в [`subsystem-map.md`](subsystem-map.md) §2.

## 4. Граница C/C++ и ABI

- **C API** (`mdbx.h`): публичный интерфейс, ABI-stable; on-disk формат не зависит от платформы.
- **C++ API** (`mdbx.h++`): тонкий типобезопасный слой поверх C-ядра.
- **C++ migration**: разворот зависимости; требует ABI test suite + signal safety.
- **Amalgamation**: `amalgamate.py` собирает всё в один файл; структурные изменения
  `#include` обязаны пройти amalgamation-gate **до merge**.

## 5. Детерминизм состояния курсора: правила позиционирования

Сформулированы при устранении неоднозначностей позиции курсора после неудачных
операций (issues #301/#304/#305/#313) и обязательны для любого изменения
навигации/позиционирования (S3).

**R1 — per-command uniformity.** Неудача команды позиционирования обязана
приводить курсор к **одному** детерминированному состоянию, определённому для
данной команды, независимо от причины неудачи и внутреннего пути исполнения
(nested tree vs sub-page, split/merge, положение точки вставки). Разные
команды могут иметь разные «неудачные» состояния, но внутри одной команды
исход един.

**R2 — determinism of follow-ups.** Из неудачного состояния все последующие
операции (`GET_CURRENT`, `NEXT*`/`PREV*`, `del`, `put(CURRENT)`) имеют
детерминированное поведение, одинаковое для всех путей, приведших к состоянию.

**R3 — no hidden data access.** Из состояния неудачного позиционирования
данные не выдаются и не изменяются, если только позиция не является
семантически валидной записью по документированному определению команды
(например, «за концом таблицы» — `z_eof_soft`-позиция на последней строке,
читаемая `GET_CURRENT` по документации флага).

**R4 — preserve the command semantics.** Состояние неудачи согласовано со
смыслом команды: miss точного совпадения (`SET`, `GET_BOTH`,
`TO_*_EQUAL`, `TO_KEY_EQUAL`) не выдаёт данных; miss «первый ≥»
(`SET_RANGE`, `GET_BOTH_RANGE`, `SET_UPPERBOUND/LOWERBOUND`) даёт позицию
первого большего либо строгое «за концом»; `GREATER_THAN`/`LESSER_THAN`
оседают на соседней записи либо возвращают `MDBX_NOTFOUND`.

**R5 — explicit marking.** Промежуточные состояния (за хвостом
мультизначений, хвост удалённых мультизначений) обязаны быть явно
пометены флагами (`z_eof_soft`/`z_eof_hard`, `z_inner_tail_deleted`) и не
могут читаться как заполненная позиция. Контракт зон младшего байта
состояния и правило «одного сравнения» fastpath/slowpath — в комментарии
перед `enum cursor_state` в `src/cursor.h` (зафиксировано `STATIC_ASSERT`).

**Опорная таблица исходов** (аудит 2026-11, `MDBX_DUPSORT`):

| Команда | Неудачный исход | Состояние | `GET_CURRENT` | `del` |
|---|---|---|---|---|
| `SET`/`SET_KEY` absent | `MDBX_NOTFOUND` | всё запрещено | `ENODATA` | `ENODATA` |
| `GET_BOTH`/`GET_BOTH_RANGE` miss | `MDBX_NOTFOUND` | всё запрещено | `ENODATA` | `ENODATA` |
| `SET_RANGE`/`UPPERBOUND`/`LOWERBOUND` past-end | `NOTFOUND`/`RESULT_TRUE` | `z_eof_soft` на последней строке таблицы | читает последнюю строку | `ENODATA` |
| `TO_KEY_EQUAL` miss | `MDBX_NOTFOUND` | `z_eof_soft` на последней строке таблицы | читает последнюю строку | удаляет её |
| `TO_PAIR_EQUAL`/`TO_EXACT_KEY_VALUE_EQUAL` miss | `MDBX_NOTFOUND` | единое past-the-tail внутреннего курсора | `NOTFOUND` | `ENODATA` |
| `TO_PAIR_GREATER/LESSER_*` miss | `MDBX_SUCCESS` | соседняя запись | читает её | удаляет её |

Любое изменение таблицы исходов — с прогоном позиционного аудита и обновлением
золотых/регрессионных тестов.

## 6. Модульные риски (matrix HIGH/MED/LOW)

HIGH = последовательная работа (module-lock конфликт); MED = координация через
ограничения; LOW = параллельно безопасно. Пересечения по
S2/S3/S4/S6/S7/S11/S14 (B+tree↔CoW↔GC↔Txn↔Cursor↔Overflow↔WAF) —
полная матрица пар в [`module-lock-matrix.md`](module-lock-matrix.md).
В amalgamated сборке подсистемы в одном файле → module-lock ≈ file-lock.
## 7. Каталог инвариантов, доказанных верификацией (2026-11, issue #485)

Кампания постанционной верификации (statement-level, 155/155 файлов, три
линзы + performance-проходы, итог в
[#485](https://github.com/Mithril-mine/libmdbx-devel/issues/485)) установила
и машинно проверила приведённые ниже инварианты. Таблица — индекс: при
ревью/рефакторинге соответствующих модулей проверяйте сохранность этих
инвариантов (нарушение любого из них — дефект). Полные доказательства и
преподы — в базе знаний shared-graph-memory (ключи `fact:core:verification-state`,
`bug:issue485:final-rollup`, `proc:verification:*`); архив кампании
(`docs/verification/` ветки `docs/engineering-verification`) расформирован
после переноса знаний.

### 7.1 Структуры данных (PNL/DPL/RKL/sort)

| ID | Инвариант |
|----|-----------|
| PNL-slack | Буфер PNL: raw-слотов ≥ 2·alloclen+3, alloclen ≥ запроса ⇒ in-buffer tmp radixsort и cmov-overread bsearch — в аллокации |
| PNL-capacity | `len = pnl[0] ≤ alloclen = pnl[-1]` для живых списков (исключение — granulation slack ≤ ~510 за ≈PL-запросом) |
| PNL-order | `wr.repnl` строго убывающий в `[MIN_PAGENO, limit)` в каждой точке потребления |
| DPL-cap/order | Блок `dpl_t` даёт ≥ 2·detent+18 слотов; дизъюнктные диапазоны, строгое возрастание sorted-головы, сентинеллы 0/P_INVALID |
| RKL-I1…I5 | rkl: пустое состояние; `empty ⇔ begin>end`; список строго возрастает; нет элементов в `[begin−1, end]`; `list_length ≤ list_limit` — сохраняются всеми мутаторами |
| L1/PAIR-N1 | `pnl_merge` сохраняет убывание; результат = сумма−1 ⇔ пара-дубликат (флаг, не счётчик); при флаге содержимое — мусор, список отбрасывается; дубликаты **внутри** одного входа ловит только `pnl_check_full` (MDBX_VALIDATION) |
| SORT-search/quicksort/radix | SEARCH_IMPL — точная lower_bound; Hoare-сканы останавливаются на pivot; stable 2-pass LSD radix с in-buffer tmp |

### 7.2 Долговечность / мета / формат

| ID | Инвариант |
|----|-----------|
| Two-phase-reader-safety | Читатель через `meta_snap()` (двойное чтение a/b + acq/rel) видит цельную старую или новую мету при всех трёх транспортах записи; false-valid требует `new ≡ old (mod 2³²)` — невозможно при +1/+2 инкрементах |
| L6 | Протокол `meta_update_begin/end`: `txnid_b←0` → `txnid_a←txnid` → `txnid_b←txnid`; bootid пишется до release-store `txnid_b` |
| meta-validate-branches | Все 14 reject/skip-ветвей `meta_validate()` полны; цепочка коррекций геометрии сохраняет `lower ≤ upper`, `fua ≤ now ≤ upper`; эквивалентности height/root/leaf/branch (issue #454) полны |
| sync-locked-exit-map | Каждый отказ `dxb_sync_locked()` ставит ENV_FATAL_ERROR **кроме** discard-scan madvise early-return (dxb.c:959, задокументированное исключение) |
| shrink-two-phase | `dxb_shrink_bottom ≥ largest ≥ fua` — shrink никогда не режет живые данные; нулевой сентинелл коллизионно-свободен |
| fetch-head-protocol | Цикл coherency #269: validate-after-read корректен; bailout 100 мс (6553 × 16.16) |
| troika-tie-rule | Полный tie тройки ⇒ старший индекс; tie-состояния = clashed (недостижимы при `meta_eq_mask == 0`) |
| pv-pages-roundtrip | `pv2pages(pages2pv(v)) == v` (машина-проверка в `mdbx_init`, CHECKING-сборки); `E∘D` идемпотентен |
| L3/troika-fsm-proof-unused | `troika_verify_fsm()` — исчерпывающее машинное доказательство FSM (216 состояний), но не вызывается нигде — обязательство: активировать ut-тестом |

### 7.3 Страничная машина / дерево

| ID | Инвариант |
|----|-----------|
| LEM-V3-1 | iov-запись нормализует disk-txnid страниц (замыкает петлю front/disk/coherency машины состояний) — load-bearing |
| PAIR-N7 | page_split root-split: сдвиг стеков курсоров ограничен `CURSOR_STACK_SIZE`; цепочка `meta_validate height < CURSOR_STACK_SIZE` ⇒ `top ≤ 30`, `top+stash ≤ 31` |
| PAIR-N9 | Машина состояний страницы `front_txnid` + запрет nested-WRITEMAP — несущие гейты `is_frozen/spilled/shadowed/modifiable` |
| PAIR-N6 | Все raw-записи `top_and_stash` в границах `[0,31]`, `stash ≥ 0` после записи |
| LEM-V4-1..5 | Branchless-поиск = точная lower_bound; сепаратор node-0 = don't-care; frozen parent ⇒ frozen children; walk-учёт байт точен для всех 5 форм страниц; combo_state packing |
| INV-V4-Z | cutoff_zikkurat: точная область вырезания, точное трёхстороннее разбиение прочих курсоров |
| LEM-V4-2 | Сепаратор ветки: node i ≥ 1 = min поддерева i; node 0 — заполнитель; refresh-at-0 в node_move/page_merge — несущий |

### 7.4 Курсоры

| ID | Инвариант |
|----|-----------|
| L7 | Зоны Z0–Z3 младшего байта flags: fastpath/slowpath одним сравнением; знак combo_state ⇔ poor/pointed (STATIC_ASSERT) |
| PAIR-N4 | Вложенный курсор всегда имеет `subcur == nullptr`; гейт `!m3->subcur` в cursor-mut.c:1122 несущий |
| PAIR-N5 | `F_ISSET` — ALL-bits: advance-ветка `cursor_del` недостижима для не-filled outer-состояний |
| INV-CC4/CC5 | Каждая явная регистрация временного курсора в `txn->cursors[dbi]` обязана париться на каждом выходе (23 сайта проверены; единственное нарушение — cursor-put-current-dangling-registration, исправлен) |
| INV-CB1..CB6 | Границы роста sub-page (subpage_limit ≤ leaf_nodemax−NODESIZE); dupfix size bound; «отравленный» txn содержит курсоры; clc pin discipline; двойная кодировка past-end; full-collapse навигация |

### 7.5 Транзакции / GC

| ID | Инвариант |
|----|-----------|
| INV-T1 | nested begin: geo/dbi_seqs/dbi_sparse снимок ДО первой падающей аллокации (#448); field-state-таблица всех failure-сайтов |
| INV-T2/L4 | `txn_shadow_cursors` при отказе восстанавливает родительские цепочки через backup для любой позиции отказа |
| INV-T3 | nested_join: все резервы (repnl/spill/dirty) выполнены до границы «дальше отказов нет» |
| INV-T5 | Между успехом `txn_write` и `dxb_sync_locked` нет падающих операций: полузаписанные страницы — недостижимый мусор до flip меты |
| INV-T6 | NOMETASYNC `txnid_dist` согласован между txn-basal и dxb_sync_locked (машина-проверено) |
| INV-T8 | unpark: POSIX-veto move при живом слоте; Windows same-base remap — проектное поведение; moved-base detection несущий |
| INV-T9 | dpl_setlen НЕ клампит `sorted` — на этом держится учёт refund/spill |
| INV-T10 | `audit_ex` — тождество страничного баланса (ловит двойное выделение и неучтённые страницы), доверяет поддерживаемой статистике |
| L5 | Аллокация `basal_txn` включает `sizeof(cursor_couple_t)` под GC-курсор |
| INV-PREPFLAG | `flags += / -= z_gcu_preparation` — точный set/clear (ни одно zone-состояние не несёт бит 0x02); + STATIC_ASSERT(ALLOC_RESERVE == z_gcu_preparation) делает GC self-reclaim гейт |
| LEM-STOCKPILE/ESCALATION | Терминация gc_prepare_stockpile(4retired): каждый успешный num=1 RESERVE растит repnl или поглощает конечный GC id; эскалация бюджета — 1/13 геометрическая сумма |
| INV-D1 | Дедупликация arcs-карты defrag через чередование walk-опций (cycle-2+ с dont_walk_MAIN) |
| INV-D2 | Defrag двигает только free/moving страницы — смещение LP самозалечивается |
| L4 | rkl_push при дубликате откатывает сдвинутый хвост (исторический off-by-one закрыт) |

### 7.6 Координация (S9)

| ID | Инвариант |
|----|-----------|
| INV-S9-1 | bind: poison-txnid → publish-pid; краш в любой точке публикации не оставляет видимой «живой» записи |
| INV-S9-2 | `rdt_length` — единственный писатель-сайт под мьютексом; все читатели через `rdt_count()` (кламп к max_readers) |
| INV-S9-3 | pid-reuse деградирует только liveness (ложная «живость» — безопасна); EOWNERDEAD-recovery проверяет всех читателей до `pthread_mutex_consistent` |
| INV-S9-4 | lckless ⇔ только RDONLY-совместимые сценарии (гейт ALL-bits, машина-проверено) |
| INV-S9-5 | fork: ребёнок реинициализирует все process-мьютексы, никогда не разрушает родительские ipc |
| INV-S9-6 | Все 10 `#error FIXME` сайтов lck-posix — compile-time flavor-гейты; автодетект выбирает только покрытые flavors |
| PAIR-N10 | cached_oldest distrust-дисциплина + терминация kick-цикла |
| PAIR-N11 | tail-refund не может обрезать живой снапшот (writer-local, без cached-oldest) |

### 7.7 API / C++

| ID | Инвариант |
|----|-----------|
| INV-CC1..CC3 | walk containment (z_pagecheck), dbi seq-last публикация (одно seq-first исключение), temp-курсоры самозациклены |
| INV-E1 | Лестницы mdbx_env_create/open/close освобождают ровно выделенное |
| INV-CACHE1 | 7-статусная машина get-cached: фильтры `> DIRTY` и `> HIT` корректны по порядку enum; CONFIRMED опирается на INV-P1 (re-stamp root) |
| INV-CXX1..CXX3 | managed-обёртки C++: матрица null-on-death/keep-on-mismatch на всех выходах; ctor exception safety делегированием; пользовательские исключения пересекают C-границу только через exception_thunk |
| LEM-V9-CACHE-SEQLOCK | MT-публикация get-cached — корректный seqlock (poison → поля → publish) |
| LEM-V9-VSIZE-CAP | `valsize_max ≤ 0x7FF00000` — валидирует `(int)`-геттеры лимитов и uint32-хранение длины кэша |
| L8 | Отказ `dxb_sync_locked()` ⇒ ENV_FATAL_ERROR (исключение — madvise discard early-return, dxb.c:959) |

### 7.8 Производительность (проверено оптимальным; негативные результаты — тоже результат)

Branchless-поиск компилируется branchless (дизасм gcc/clang, барьер
`CLEAR_VALUE_PROPAGATION` несущий); sort.h cmov-bsearch быстрее классики на
всех размерах 16..8192; DPL 42-window pivot, txn_write compaction
(error-path-only), oldest-scan early-break + 4-pass bound, cursor
page-walks (формат-ограничены), scan4seq SIMD dispatch, prefault batching,
dense-hist редкость, rkl cost model, comparators (UNALIGNED_OK single
loads), mdbx.h inline layer, C++-обёртки zero-overhead (асс-проверка).
