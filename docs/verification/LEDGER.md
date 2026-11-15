# Verification LEDGER — libmdbx master `b5ba3018`

Кампания постанционной верификации: каждое условие и каждая арифметическая
операция получают доказательство корректности или контрпример.
Протокол обязательств P1–P7, вердикты и **обязанности агентов** (включая
пополнение БЗ shared-graph-memory и подготовку дополнений в инженерную
документацию) — `docs/verification/protocol.md`.

**Вердикты:** `PROVED` · `PROVED·A` (с допущениями, каждое — обязательство) ·
`REFUTED` (контрпример + репродьюсер в `repro/`) · `SUSPECT` · `N/A`.

**Потоки:** A «Леммы» (снизу-вверх), B «Адверсарий» (по-риску),
C «Пары структура→потребитель». Пересечения намеренны; расхождения вердиктов
между потоками обязательны к повторной верификации.

**Факты-константы (опорные для всех доказательств):**
- `MDBX_PNL_ASCENDING == 0` (options.h:740) — PNL по убыванию, ASCENDING-ветки мёртвые;
- `pnl[0] == pnl_size`, элементы `pnl[1..len]` по убыванию, `alloclen ≥ len`;
- `MIN_PAGENO == NUM_METAS == 3`; `MDBX_PNL_SIZEOF = (len+1)·sizeof(pgno_t)`;
- `CURSOR_STACK_SIZE = 16 + MDBX_WORDBITS/4`; `MDBX_DATA_VERSION = 3`;
  `sizeof(reader_slot_t) == 32` (STATIC_ASSERT);
- `MDBX_ENABLE_BIGFOOT=1` (по умолчанию), FIFO — режим GC по умолчанию.

---

## Реестр лемм

| ID | Лемма | Источник |
|---|---|---|
| L1 | `pnl_merge()` сохраняет убывание; результат = сумма длин −1 ⇔ найдена пара-дубликат (флаг, не счётчик); при флаге содержимое — мусор, список подлежит отбрасыванию | pnl.c:191–255; PAIR-N1 (пары-wave1) |
| L2 | `rdt_count()` клампит внешне-перезаписываемое `rdt_length` к `max_readers` на каждом чтении | internals.h:747 |
| L3 | `troika_fsm_map` исчерпывающе верифицируется `troika_verify_fsm()` (216 комбинаций) — но функция не вызывается нигде (V-001) | meta.c:85–131 |
| L4 | `rkl_push()` при дубликате откатывает сдвинутый хвост; инвариант `rkl_check` восстанавливается | rkl.c:296–307; подтверждено G1-dpl-rkl |
| L5 | Аллокация `basal_txn` включает `sizeof(cursor_couple_t)` под GC-курсор | txn-basal.c:74 |
| L6 | Двухфазный протокол меты: `txnid_b←0` → `txnid_a←txnid` → `txnid_b←txnid`; `meta_snap()` видит цельную мету | meta.h:96–124, meta.c:21–30 |
| L7 | Курсор: знак `combo_state` ⇔ poor/pointed; зоны Z0–Z3 фиксируют fastpath/slowpath одним сравнением | cursor.h:88–171 (STATIC_ASSERT) |
| L8 | Любой отказ `dxb_sync_locked()` ставит `ENV_FATAL_ERROR` — **исключение V-025** (discard-scan madvise early-return, dxb.c:959) | dxb.c:1223–1225; wave2.md |
| PNL-slack | Буфер PNL: raw-слотов ≥ 2·alloclen+3; alloclen ≥ запроса; значит in-buffer tmp radixsort и cmov-overread — в аллокации | G1-pnl-dml-txl (Lemma) |
| PNL-capacity | len = pnl[0] ≤ alloclen = pnl[-1] для живых списков; исключение — granulation slack (V-008) | G1-pnl-dml-txl |
| PNL-order | repnl строго убывающий в [MIN_PAGENO, limit) в каждой точке потребления; продюсеры — order-preserving или followed-by-sort | G1-pnl-dml-txl |
| detent≥seq | На всех call-sites pnl_get_best_sequence defrag_enough > payload_pages ≥ любого seq | G1-pnl-dml-txl |
| SORT-search | SEARCH_IMPL: точная lower-bound позиция в [begin,end]; cmov-overread в аллокации, результат-нейтральный | sort.h:436-482 |
| SORT-quicksort | Hoare-сканы останавливаются на pivot (median-of-3 сеть); глубина стека ≤ 33 | sort.h:276-344 |
| SORT-radix | ≤2 прохода для 32-битных ключей; stable LSD; tmp полностью записан до чтения | sort.h:349-412 |
| DPL-cap | Блок dpl_t даёт ≥ 2·detent+18 слотов; length ≤ detent; все записи ≤ detent+2 | G1-dpl-rkl |
| DPL-order | Дизъюнктные диапазоны страниц; строго возрастание в sorted-голове; сентинеллы 0/P_INVALID | G1-dpl-rkl |
| RKL-I1..I5 | rkl: пустое состояние; empty ⇔ begin>end; список строго возрастает; нет элементов в [begin−1, end]; list_length ≤ limit — сохраняются всеми мутаторами | G1-dpl-rkl |
| PAIR-N4 | Вложенный курсор всегда имеет `subcur == nullptr`; гейт `!m3->subcur` в cursor-mut.c:1122 несущий | pairs-wave1 (Pair 3) |
| PAIR-N5 | `F_ISSET` — ALL-bits: advance-ветка cursor_del недостижима для всех не-filled outer-состояний | pairs-wave1 (Pair 3) |
| PAIR-N6 | Цепочка height-bound: meta_validate + cursor-setup + cursor_push ⇒ top ≤ 30, top+stash ≤ 31; все raw-записи top_and_stash в границах | pairs-wave1 (Pair 4) |

---

## Специмены (эталон глубины)

### SPEC-1: `pnl_cut()` — pnl.c:377 — **PROVED** (подтверждено повторно обоими агентами потока A)
ASSERT-границы доказаны перечислением всех 3 call-sites; `len -= span` без underflow (span ≤ len+1−pos ≤ len); направление копирования overlap-safe; терминация; ветка `span==0` недостижима по перечислению call-sites (мёртвая защитная ветка). Полное доказательство — в каждом из отчётов G1.

### SPEC-2: `meta_cmp2pack()` + `troika_fsm_map` — meta.c:43–131 — **PROVED (machine-checked), доказательство мёртво (V-001)**
`troika_verify_fsm()` — исчерпывающее машинное доказательство (216 комбинаций, независимый пересчёт всех полей). Находка V-001: функция не вызывается ни в одном TU → обязательство активировать (ut-вызов).

---

## Реестр находок (глобальный; V-### — канонический ID)

| V-id | Где | Суть | Серьёзность | Поток |
|---|---|---|---|---|
| V-001 | meta.c:85 | `troika_verify_fsm()` не вызывается — машинно-проверяемое доказательство FSM мёртвым кодом | low | A(коорд.) |
| ~~V-002~~ | gc-get.c:469 | **ОПРОВЕРГНУТ (false positive, ретракция)** — харнес волны-1 содержал путаницу элементов/байтов: `ptr_disp` в коде даёт **байтовое** смещение, и `clz>>2` = 12−4b байт = ровно корректная позиция для всех лейнов; 0xFFFF-поля эмпирически подтверждены (selftest 0x0000ffff00000000); 3,670,024 исчерпывающих случаев на реальном NEON (qemu aarch64+armhf × обе ветви хвоста) — 0 расхождений; long-seq (seq≤~1190) — 0. См. v002-retraction.md | — | B→отзыв |
| **V-003** | gc-put.c:418 | **REFUTED (high):** BIGFOOT `gc_store_retired`, FIFO — `at = left_before − chunk` underflow при росте retired_pages внутри окна `cursor_put(RESERVE)` (`page_touch` frozen-страницы): OOB write до блока malloc + порча/брик GC-записи → CORRUPTED или тихое двойное выделение страниц; LIFO иммунен; фикс-форма: `retired_after − chunk` | **high** | B |
| **V-004** | gc-put.c:678-710 | **REFUTED (liveness):** `solve_recursive` — бинарный поиск с рекурсивным предикатом, работа ≈ (log₂N)^H; контрпример: 84M рекурсивных вызовов (~45s) до FALSE; граница loop-42 не действует; поток коммиттера (writer mutex) замирает без таймаута — DoS-уемо | medium | B |
| V-005 | pnl.h:65; dml.c:26 | ASSERT-верхняя граница bytes2size не учитывает до-делительный размер (≈2×запрос) — ложный assert в checking-сборках при ≈PL/2+slack; release корректен | low | A |
| V-006 | pnl.c:336 | `defrag_detent − seq` wrap при detent < seq — в функции не защищено; на всех 3 call-sites доказано невозможно; латентная опасность для будущих вызовов | low | A |
| V-007 | defrag.c:746 | `pnl_cut_range` rc игнорируется (на сайте доказуемо безотказно) — гигиена unchecked-error | low | A |
| V-008 | pnl.c reserve/need | Granulation slack: alloclen может превысить PAGELIST_LIMIT на ≤ ~510; len может перевалить; check_lite/full отвергнет самопостроенный список (debug); release безвреден | low | A |
| V-009 | pnl.h:195-203 | `pnl_contains_span`: cmov-overread pnl[len+1] может дать ложное «true» на устаревшем байте; использование только ASSERT-овое с известным pgno ⇒ benign | info | A |
| V-010 | pnl.c:198/218; gc-get.c:1189 | Детекция дубликатов выключается `MDBX_DISABLE_VALIDATION=1`; строгий порядок входов merge гарантируется только при runtime MDBX_VALIDATION | info | A+C |
| V-011 | pnl.c:387/262; txl.c | Мёртвые экспорты: pnl_sift (0 вызовов), pnl_intersect (tests-only), txl_* (tests-only, продакшн использует rkl) | info | A |
| V-012 | pnl.h:167-169 | Null-толерантный ASSERT непосредственно перед безусловным deref — вводящий в заблуждение гард | info | A |
| V-013 | dml.c:92-102 | cmov-overread items[0] без Valgrind-аннотации (как в pnl_search) | info | A |
| V-014 | dpl.c:24/59/75-78/84 | Micro-map (pagesize=256, upper=1024) ⇒ `wanna=4 < CURSOR_STACK_SIZE` — ложный assert в checking-сборках; release safe (granulate floor ≥ 502) | low | A |
| V-015 | rkl.c:215-218 | Проба `rkl_extend_solid_gap_head` недостижима при I4 — мёртвая защитная инструментация | info | A |
| V-016 | rkl.c:615-644 | Контракт rkl_hole: терминальная дыра повторно выдаётся при повторных вызовах; однопроходный контракт соблюдается gc_search_holes | info | A |
| V-017 | rkl.c:445-449 | `rkl_destructive_merge` уничтожает src даже при ошибке; in-tree вызовов нет — опасность только при переиспользовании | info | A |
| V-018 | gc-put.c:322 (+txn-nested.c:324) | `pnl_merge(repnl, loose)` — результат игнорируется (в отличие от gc-get-дисциплины); опирается на структурную дизъюнктность; дешёвая defense-in-depth: сравнить с `pnl_size(repnl)+count` | low | C |
| V-019 | cursor-nav.c:349/388; cursor-seek.c:673; tree-cutoff.c:243/302 | Raw-записи `top_and_stash = (uint16_t)level` отбрасывают stash без assert `stash==0`; безопасно для settled-курсоров, инвариант не enforced | low | C |
| V-020 | tree-ops.c:587-589 | `top_and_stash = save_top − delta` требует структурного `delta ≤ save_top` — только assert | low | C |
| V-021 | spill.c:393-410 | При отказе spill_page запись выкидывается из DPL не будучи записанной/спилленной; accounting самосогласован; безопасно только пока все failure-пути abort'ят txn | info | C |
| V-022 | gc-get.c:923-939; pnl.c:153-166 | Crafted GC-запись с внутренним беспорядком (`[12,99,11,9,10]`) проходит check_lite в не-VALIDATION сборке → двойное выделение страницы 10; задокументированный компромисс «release не платит за crafted-data hardening» | low | B+C |

**Кросс-потоковая сверка Волны 1:** B и C независимо верифицировали pnl_merge — сходимость (sentinel-защита «attacked→held» у B; garbage-on-flag PAIR-N1 у C — дополняют друг друга). A1 F-6 = C PAIR-F2 = B defense-4 — одна и та же находка V-010 от трёх потоков. Противоречий вердиктов нет.

---

## Волна 2 (см. wave2.md; полные леджеры — doc-additions-v2.md, doc-additions-v6-b.md, doc-additions-v2c.md)

**Канонические находки Волны 2** (перенумерованы; в отчётах потоков могли фигурировать локальные номера):

| V-id | Где | Суть | Серьёзность | Поток |
|---|---|---|---|---|
| V-023 | dxb.c:354–355 | 32-bit wrap `pv2pages(grow/shrink_pv)·pagesize` в geometry-from-db (не ограничено meta_validate) — PROBE-3 | low | A |
| V-024 | dxb.c:52–55 | Слепое пятно discovery pagesize: повреждённая meta-0 + pagesize>sys ⇒ валидные meta-1/2 не пробуются ⇒ CORRUPTED при 2 целых метах — PROBE-4 | low | A |
| V-025 | dxb.c:959 | Отказ madvise в discard-scan выходит без ENV_FATAL_ERROR — исключение из L8 | low | A |
| V-026 | meta.c:195 | troika dump: tail печатается как `%3`, биты флагов протекают (log-only) | info | A |
| V-027 | dxb-resize.c:475 | readahead_anchor переполняется pgno_t при edge=2³¹ (advisory) | info | A |
| V-028 | dbi.c:273 + txn-nested.c:701 + txn-basal.c:271 | Отказ dbi_lock в dbi_update: nested-коммит теряет rename-undo; basal-abort оставляет undo на переиспользуемом txn | low | B |
| V-029 | txn.c:392 vs txn-basal.c:66 | Layout-drift **опровергнут** как активный баг (INV-T7); опасность только при переиспользовании — хардендинг рекомендован | info | B |
| V-030 | txn.c:164–173 | Latency-снапшот в txn_abort против общего pgops — теоретически безграничен | info | B |
| V-031 | dxb.c:590 (+389-395, 640-644) | bootid-match ветка dxb_setup теряет user-amended geometry при переоткрытии | low | C |
| V-032 | cursor-seek.c:100+115 vs 152-159/280 | SET_RANGE past-end: две кодировки (ki=nkeys−1 vs ki=nkeys) ⇒ GET_CURRENT расходится — разрыв R1 | low | C |
| V-033 | cursor-seek.c:516-517 | TO_KEY_EQUAL miss оставляет курсор filled на соседней записи — строка таблицы §5 покрывает только past-end | low/doc | C |
| V-034 | txn-ro.c:195/121/284; txn-basal.c:253 | Подделка cached_oldest_txnid: RW восстанавливается, RO — фатальный ENSURE (DoS читателя до writer-rescan) | low | C |
| V-035 | cursor-seek.c:676, 844 | eof_hard rank в cursor_distance_at при ki=nkeys: wrap distance — release-safe, assert в checking | low/info | C |
| V-036 | mvcc-readers.c:394-502 | hsr-decline цикл: INT_MAX-граница, практически безграничен (семья V-004) | info | C |

**Новые леммы/инварианты Волны 2:** two-phase-reader-safety (все 3 транспорта записи; false-valid требует new≡old mod 2³²; amd64 — cacheline containment txnid_a@28/txnid_b@196); pv-pages-roundtrip (D∘E∘D=D, машина-проверка global.c:298); meta-validate-branches (14 ветвей); sync-locked-exit-map; fetch-head-protocol (#269 + bailout 100мс); shrink-two-phase-arith (bottom ≥ largest ≥ fua — live data не режется); troika-tie-rule (полный tie ⇒ старший индекс; tie = clashed, недостижимо); histogram-packing; INV-T1..T10 потока B (nested-begin-fault — #448 подтверждён полным; cursor-shadow-restore; nested-join-infallible; basal-commit-failure — между txn_write и dxb_sync_locked нет падающих операций; nometasync-txnid-dist — машина-проверка; txn-layout-drift; unpark-base-aba; dpl-setlen-no-clamp; audit-identity-model; spill-lsb-abort); PAIR-N7..N11 потока C (page_split stash-bound; page_get ILL dispatch; front_txnid state machine + запрет nested-WRITEMAP; cached-oldest distrust + kick termination; tail-refund reader safety).

**Кросс-потоковая сверка Волны 2:** противоречий нет; C и A сошлись по legal4overwrite/two-phase; B INV-T9 подтвердил dpl.h-вердикт A; V-036 семья V-004.

---

---

## Волна 3 (см. wave3.md; полные леджеры — doc-additions-v3.md, doc-additions-v5-b.md, doc-additions-v3c.md)

**Канонические находки Волны 3:**

| V-id | Где | Суть | Серьёзность | Поток |
|---|---|---|---|---|
| **V-037** | api-txn-data.c:301 vs :282-283/:311 | **HIGH, ASAN-подтверждено:** `mdbx_put` MDBX_CURRENT size-precheck делает голый `return MDBX_BAD_VALSIZE` в обход анрегистрации курсора → висячий зарегистрированный курсор (стек-фрейм) в `txn->cursors[dbi]` → stack-use-after-return при любом последующем cursor op / txn_done_cursors. Репро: repro/v3c-put-dangling-cursor.c. Правило INV-CC4 | **high** | C |
| V-038 | tree-ops.c:25/:87 | **Поломка сборки** `MDBX_ENABLE_BUNCHES_REMOVAL=0`: переименование cursor_pop→cursor_eject (86eb2a15) пропустило #else-ветку tree_drop; gcc -fsyntax-only воспроизводит | medium | C |
| V-039 | page-split.c:507-519 | Формальный UB: root-split сдвиг других курсоров ограничен старой высотой дерева, не m3->top — при максимальной валидной высоте пишет m3->pg[CURSOR_STACK_SIZE] (один за pg[], алиасит ki[0], немедленно перезаписывается — практически benign); машина-проверка PROBE-1; фикс: граница по m3->top | low | A |
| V-040 | page-ops.c:770-786; page-iov.c:157 | Неудовлетворимый eASSERT в recalculate_subpage_thresholds (user-reachable); large-page write может превысить MAX_IO_BYTES (release-safe через clamp+partial-write) | low/info | A |
| V-041 | node.c:210-215/282-287/136-145 | Асимметрия харденинга: guard node_del против memmove-underflow — только checking, первичный guard VALIDATION-гейтится (в то время как sub-pages получили hot-path subpage_geometry_ok); crafted offset < upper → underflowed memmove в default-сборке (доверенная модель A7) | info | A |
| V-042 | page-ops.c:128; cursor-mut.c:335/342 | page_unspill PROBLEM и shadowed-large PROBLEM не ставят MDBX_TXN_ERROR — латентно (для легитимных штампов недостижимо) | info | A |
| V-043 | cursor-seek.c:100+115/152+280/676 vs api-range-estimate.c | Прошло-конец-двойная кодировка → mdbx_estimate_distance=−1, mdbx_cursor_distance(deepness=0)=−2 (size_t wrap), mdbx_estimate_range=0 на одном и том же логическом диапазоне — тихо под ассертами; семья V-032/V-035, унифицирована INV-CB5 | low | B |
| V-044 | cursor-seek.c:748-764 | Дистанция по multi-page nested trees без внутриви-скидки ранга (in-code признано) | info | B |
| V-045 | cursor-mut.c:887-888 | clc env-global → кросс-txn липкий pin lmin/lmax (контракт uniform-size, формально racy но aligned-size_t консистентно) — INV-CB4 | doc | B |
| V-046 | cogs.h:318 vs leaf2 pages | Настоящие leaf2-страницы лишены release-mode geometry-проверки, которую имеют sub-pages — сегодня только mdbx_chk | info | B |
| V-047 | txn-nested.c:501 (nested_undo→dbi_update(false)) | Расширение V-028: nested-abort также выходит при отказе dbi_lock — rename-undo теряется вместе с освобождённым nested txn; rename не откатывается (kvs/MAIN desync) | low | C |
| V-048 | cursor-mut.c fix_parent | Отсутствие MDBX_TXN_ERROR в fix_parent компенсируется page_split bailout (R-C1 — опровержение опасности) | info | C |

**Новые леммы/инварианты Волны 3:** LEM-V3-1 (iov-page disk-stamp normalizer — замыкает петлю PAIR-N9 front/disk/coherency; load-bearing), LEM-V3-2 (dupfix reserve самосвязанность, перебор 5.1M состояний), LEM-V3-3 (tmp-scan: учётная идентичность, нет underflow, терминация), LEM-V3-4 (точность tmp-return memcpy через инвариант нод-стека), LEM-V3-5 (терминация move-loop + spill-защита mp stash'ем), LEM-V3-6 (формула минимума page_check: branch-точна, leaf-slack = один zero-key node, ловится по-нодово), LEM-V3-7 (бескollisionность подписи is_tmp), LEM-V3-8..10; INV-CB1..CB6 (рост sub-page ограничен; dupfix size bound; заражённый-txn курсор контейнера; clc pin discipline; унификация болезни dual-encoding; полная-collapse навигация); INV-CC1 (walk containment chain: z_pagecheck ⊥ walk size loops — load-bearing), INV-CC2 (dbi seq-last публикация с одним seq-first исключением), INV-CC3 (temp couples самозациклены; явная регистрация обязана париться на каждом выходе), INV-CC4 (правило ревью из V-037); предположения A1..A7 (регистр потока A).

**Кросс-потоковая сверка Волны 3:** противоречий нет; B V-043 + C V-032/V-035 — один корень (dual encoding), унифицирован; A V-039 фикс также снимает опасение V-019 на этом сайте; C R-C1 подтверждён полным перебором bailout-ветвей A.

---

---

## Волны 5–6 + поток D «Оптимизатор» (см. wave5-6.md; 8 агентов параллельно)

**Канонические находки V-055..V-077** (полные таблицы — wave5-6.md):

| V-id | Где | Суть | Серьёзность |
|---|---|---|---|
| **V-055** | api-opts.c:209-228 | MDBX_opt_sync_bytes: `(uint32_t)` без проверки страниц → set 2^44 даёт threshold 0 (машина-доказано, репро) | low-med |
| V-062..V-064 | api-gc.c:166-171; defrag.c:86-93 | defrag #55 контракты: stuck-retry возвращает SUCCESS+noobstacles; **time_limit игнорируется при недостижимой цели** (машина-проверено, рефутация docs); adopted-txn коммитит записи пользователя mid-txn (недокументировано) | doc/contract |
| **V-065** | osal/windows/osal-mmap.c | **major-if-confirmed (win-reading):** RDONLY mapping extent фиксируется на момент открытия → рост БД за пределами ⇒ EPERM ⇒ mdbx_txn_begin падает у читателя | major-if-confirmed |
| **V-067** | load.c readline/readhdr | NUL-байт молча обрезает строку дампа → фантомная запись, хвост потерян, exit 0 (машина-доказано) | P3 |
| **V-068** | load.c:787-797 | `-p` purge падает INCOMPATIBLE на хвостовом MAIN каждой multi-table выгрузки → частичная загрузка + exit 1 (машина-трасса) | P2 |
| V-056..V-061, V-066, V-069..V-077 | api/tosal/tools/cxx | Семейства minor/info/doc: checkpoint-latency, env_info zeroed, estimate wrap, key2value UB-doc, −INT64_MIN, depthmask shift; osal windows minors; stat -r exit-codes; drop abort-on-terminated; logger fmt/isxdigit; cxx: from_hex("")/is_printable 0xE0-EF/inline-API missing fallback (V-076 — единственный из 27 inline без экспорта)/мелочи | low/info |

**Рефутации-гипотез (машина-проверено):** amend TRY+PARKED (R-V9-1), checkpoint-освобождение nested-child (V13a), double-munmap lck (R-C3, strace), V-038-batch (V13a cursor accounting).

**Производительность (поток D):**
- **P-E1** (LOW risk, hot): скалярные tail-shift циклы pnl_cut/gc_repnl_get_sequence/refund_reclaimed в 5-10× медленнее memmove на длинных хвостах — гибридный memmove >32 элементов;
- **P-E2** (LOW-MED): pnl_insert_span фронт-вставки O(len) → bsearch+memmove;
- **P-E3** (MED-HIGH, benchmark-gated): branchless pnl_merge_inner ~10-12 c/elem против branchy ~4.6× быстрее на fair-random данных — вопрос алгоритма, не codegen (flag #83/#452 несущий);
- **P-A1** (none risk): dump.c putchar() с локом на байт → putc_unlocked (2-4×);
- **P-A2/P-A3**: get_batch staging-копии (API shape); SingleThreaded doc-неоднозначность;
- **Негативные результаты (проверено оптимально):** branchless-поиск компилируется branchless (дизасм), sort.h cmov-bsearch быстрее на всех размерах, DPL/rkl/mvcc/iov/comparators/mdbx.h-inline — OPTIMAL (INV-PERF-1..7); C++ zero-overhead машина-проверено (INV-PERF-CXX1); кэш HIT lockless, CONFIRMED O(1) (INV-PERF-CACHE1).

**Новые леммы/инварианты:** LEM-V9-CACHE-SEQLOCK, LEM-V9-FALLBACK-DEPTH, LEM-V9-BUNCH-PAIRING, LEM-V9-DEFRAG-OWNERSHIP, LEM-V9-AMEND-NO-LOCK-LEAK, LEM-V9-VSIZE-CAP, LEM-V9-NULL-NAME; INV-D1 (defrag arcs-учёт через alternation dont_walk_MAIN), INV-D2 (defrag двигает только free/moving — самозалечивание), INV-C2/C3 (chk ledger + GC-record containment), INV-OS1..OS6 (ior slots, windows item encoding, drain contract, fd parking, mresize decode, RDONLY extent asymmetry); INV-T1/T2 (dump partition/concise grammar), LEM-V12-hex/readline-bounds/maxdbs2/commit-contract; INV-CXX1..3 (managed handle matrices, ctor exception safety, exception thunk), LEM-CXX1..5, FACT-CXX-API.

**Кросс-потоковая сверка:** противоречий нет; V13a/V13b подтвердили exception-thunk и ODR с обеих сторон; P-E1/P-E2 сайты несут волновые-1 вердикты корректности — рекомендации shape-preserving; P-E3 явно не меняет код без workload-бенчмарка.

---

## Транши (финал)

| Транш | Файлы | Статус |
|---|---|---|
| V1, V7 | G1 структуры; G7 GC | ✅ волна 1 |
| V2, V6, Пары-1..2 | G2; G6; dpl→txn_write/spill; meta→dxb; page-ops→split/get; cursor.h→cursor-mut | ✅ волна 2 |
| V3, V5, Пары-3 | G3; G5+tree-search; tree-ops→cursor-mut; walk→chk-walk/api-copy; dbi→table; tree-search→estimate | ✅ волна 3 |
| V4, V8, Пары-4 | G4; G8 координация; INV-CC4 sweep (23 сайта); env-лестницы; park/clone/info; gc_info/cache | ✅ волна 4 |
| V9 | G9 API-поверхность (14 файлов) | ✅ волна 5 (A) |
| V10 | foundation + env.c + logging + defrag + chk | ✅ волна 5 (B, адверсарий) |
| V11 | G10 osal posix+windows | ✅ волна 5 (C) |
| V12 | G11 tools | ✅ волна 6 (A) |
| V13a/b | G12 C++ core + headers | ✅ волна 6 (B/C) |
| Perf-D | engine hot paths; API/C++/tools | ✅ волны 5-6 (D×2) |
| A-top-up | gc-get/gc-put/txn*/lck*/mvcc пофункционально statement-level | ⏳ волна 7 (адверсарные лензы уже прошли; осталось добить P1-P7 полноту) |

**Метрики после волн 5-6 + D:** функций/областей с вердиктами ≈1,100+; обязательств проверено ≈7,000 из 9,711; находок суммарно ≈40+ (16 машина-проверено), включая 3 high (V-002 NEON critical, V-003, V-037) и 1 major-if-confirmed (V-065); производительность: 6 действенных рекомендаций + 15+ опровергнутых (проверено-оптимально).
---

## Волна 7 — финальный A-top-up (см. wave7.md; doc-additions-v7-gc.md, doc-additions-v7-txn.md, doc-additions-v7-coord.md)

**0 новых REFUTED.** Транзакционное ядро теперь закрыто обеими линзами. Формализованы: P5-матрица выходов gc_alloc_ex (13 fail-сайтов), LEM-SCAN4SEQ-FB, LEM-STOCKPILE, LEM-ESCALATION, INV-PREPFLAG, LEM-TXN-1..4, LEM-V7C-1..3. Nit-находки: F-V7-1 (dbg `id` uninit, debug-only), N-V7-2 (sentinel wrap → scan_threshold=0, perf-only), F-TXN-1 (KB-поправка штампа nested_merge), F-TXN-2..8 (info), **F-V7C-1** (low: post-fini повторное использование rthc — API-lifecycle нарушение, вне поддерживаемых сценариев).

## КАМПАНИЯ ЗАВЕРШЕНА — финальные метрики

| Метрика | Значение |
|---|---|
| Файлов покрыто | 155/155 (100%): statement-level леджер + адверсарная линза на risk-ранжированном подмножестве + контрактные сверки всех интерфейсов + perf-вердикты на всех горячих путях |
| Функций/областей с вердиктами | ~1,250 |
| Обязательств проверено | ≈8,900 из 9,711 (остаток — logging-blanket и N/A debug-only, разряжены по протоколу) |
| REFUTED-дефекты | 9 (после ретракции V-002): **V-003 high** (gc_store_retired FIFO underflow → порча/брик GC), **V-037 high** (mdbx_put висячий курсор, ASAN stack-use-after-return), V-004 medium (dense-solver liveness DoS), V-038 medium (BUNCHES=0 build break), V-023/24/25/39/53 low. Ложных срабатываний отозвано: 1 (V-002, см. v002-retraction.md) |
| Машина-проверено | 16 находок с репродьюсерами в docs/verification/repro/ |
| Контрактные/doc-находки | V-060..V-064 (defrag #55), V-065 (win RDONLY major-if-confirmed), V-049..V-052, V-067..V-077 (tools/cxx), V-055..V-061, F-V7-*/F-TXN-*/F-V7C-1 nit-семьи |
| Производительность | P-E1 (LOW, hot: pnl tail-shift → memmove), P-E2, P-E3 (benchmark-gated), P-A1..A3; **15+ проверено-оптимально** (branchless codegen машина-подтверждён, C++ zero-overhead, кэш HIT lockless) |
| Леммы/инварианты установлено | 60+ (LEDGER + doc-additions-*) — переиспользуемая база доказательств для будущих рефакторингов |
| Коммиты кампании | c11b5945, fa99524a, 9ad900f4, 8db0b3ec, 3b4878b0, + финальный |

**Приоритет исправлений (после ретракции V-002):** V-003 → V-037 → V-065 (Windows-подтверждение) → V-068/V-067 (tools) → V-004 → V-055 → P-E1 → остальное по severity.
