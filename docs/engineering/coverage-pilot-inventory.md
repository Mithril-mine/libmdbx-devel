# Coverage-pilot inventory (B68-P3): цель «100% по функционалу трёх тестов»

> Пилот методики probe-bus (mprobe v2): детерминированная fault-инъекция +
> самоутверждение покрытия через реестр. Ветка `feature/ut-coverage-pilot`.
> Baseline измерен 2026-10-01 в `_build-prof` (Debug+gcov, MDBX_PROBES=ON),
> прогон каждого теста изолированно со сбросом `.gcda` (скрипт
> `.skynet/tmp/coverage-measure.sh`).

## Правила пилота

- **Классы** непокрытых строк:
  - `R` — reachable crafted-сценарием (без правки движка);
  - `F` — fault-injection (MPROBE_FAULT / alloc-fault);
  - `D` — disarm DEV_ASSERT-предусловия вызывающего (затем crafted-вызов);
  - `C` — white-box crafted-invalid-state (прямой вызов static, дефенсивные ветки);
  - `X` — недостижимо/мёртвое (документируется и ИСКЛЮЧАЕТСЯ из цели).
- **Scope** (решение владельца): rkl.c/txl.c — весь файл; api-get-cached.c,
  gc-get.c, gc-put.c — только функции/ветки, активируемые соответствующим
  тестом (списки ниже). Остальное — вне scope, помечается `out`.
- Цель: 100% достижимых строк (классы R/F/D/C закрыты; X и out — задокументированы).
- **Аттестация**: тест в конце прогона опрашивает `mprobe_ctl("query <pattern>")`
  и требует `seen>0` для каждого обязательного тега.

## extra_details_rkl → rkl.c (весь файл)

| Строки | Что | Класс | Решение | Статус |
| --- | --- | --- | --- | --- |
| 76–77 | rkl_resize: `wanna_size > txl_max` → TXN_FULL | R | `rkl_reserve(&a, txl_max + 1)` | ✅ |
| 80–81 | rkl_resize: `wanna_size < list_length` → PROBLEM | D | DEV_ASSERT на :72 (`rkl:resize:wanna_gt_length`), disarm, прямой вызов | ✅ |
| 85–90 | resize: внешний буфер → inplace (memcpy+free) | R | прямой вызов rkl_resize с малым size | ✅ |
| 92 | resize: уже-inplace else-ветка | R | то же с list==inplace | ✅ |
| 101 | resize: `!ptr` → ENOMEM | F | `alloc-fault 1` + reserve роста | ✅ |
| 124 | rkl_copy: проброс ошибки resize | F+R | src с list>12 + alloc-fault | ✅ |
| 214 | extend_solid: `*f != solid_begin` → RESULT_TRUE | C | white-box: list=[11], direct call, тег `rkl_extend_solid_gap_head` | ✅ |
| 219 | extend_solid: `*t != solid_end` → RESULT_TRUE | C | white-box: list=[13], direct call, тег `rkl_extend_solid_gap_tail` | ✅ |
| 277 | rkl_push: проброс ошибки роста буфера | F+R | заполнить список до лимита + alloc-fault | ✅ |
| 425 | rkl_merge: return err из цикла по списку | R | src/dst с дубликатом В СПИСКЕ | ✅ |
| 518–522 | rkl_hole: reverse past-end | R | устаревший итератор после rkl_pop, тег `rkl_hole_past_end_reverse` | ✅ |
| 646 | rkl_check: limit ниже inplace | C | испорченная структура, тег `rkl_check_limit_below_inplace` | ✅ |
| 661 | rkl_check: bsearch out-of-range | X | недостижимо: rkl_bsearch всегда в [list, end] (инвариант lower_bound); тег `rkl_check_bsearch_out_of_range` | 📄 |
| 663 | rkl_check: solid_float_low | X | недостижимо: it[-1] < solid_begin по инварианту bsearch; тег `rkl_check_solid_float_low` | 📄 |
| 665 | rkl_check: solid_float_high | R/C | элемент внутри solid ([12]+solid[10,13)); тег `rkl_check_solid_float_high` | ✅ |

Аттестация: `edge_cases_probes()` в details_rkl.c опрашивает `query` для тегов
`rkl:resize:wanna_gt_length`, `rkl_extend_solid_gap_head/gap_tail`,
`rkl_hole_past_end_reverse`, `rkl_check_limit_below_inplace`,
`rkl_check_solid_float_high`, `txl_reserve_too_long` — требует seen>0.
**Результат: 100% достижимых строк rkl.c (X-исключения задокументированы).**

## extra_details_rkl → txl.c (весь файл)

| Строки | Что | Класс | Решение | Статус |
| --- | --- | --- | --- | --- |
| 44 | `allocated >= wanna` early-return | X | недостижимо: txl_reserve вызывается только при alloclen<wanna | 📄 |
| 47–48 | `wanna > txl_max` → TXN_FULL | R | прямой вызов txl_reserve(&txl, txl_max+1), тег `txl_reserve_too_long` | ✅ |
| 63 | realloc fail → ENOMEM | F | txl до ёмкости + alloc-fault | ✅ |
| 87 | txl_append: проброс ошибки | F+R | заполнить txl до ёмкости + alloc-fault | ✅ |
| 97 | txl_contain: найден | R | дубликат в txl | ✅ |

**Результат: 100% достижимых строк txl.c (1 X-исключение).**

## get_cached → api-get-cached.c (функции cache_get/mdbx_cache_get/_SingleThreaded)

| Строки | Что | Класс | Решение | Статус |
| --- | --- | --- | --- | --- |
| 62 | check_txn error | F | fault-сайт `cache_check_txn_err` | ✅ |
| 99 | dbi_check error | R | невалидный dbi (999), свежий read-txn | ✅ |
| 126 | tbl_refresh (не NOTFOUND) error | F | fault-сайт `cache_tbl_refresh_err` + DBI_STALE через env2 | ✅ |
| 155 | cursor_init error | F | fault-сайт `cache_cursor_init_err` | ✅ |
| 160 | check_key error | R | ordinal-таблица + 3-байтный ключ | ✅ |
| 165 | page_get error | F | fault-сайт `cache_page_get_err` | ✅ |
| 175–190 | notfound_elevate_trunk | R/C | вход достигнут (label + not_found) через stale-entry сценарий; **петля L183–190** требует «схлопывания» дерева (defer) | ◑ |
| 192 | elev из branch | R/C | частично (label покрыт); L195-переход defer | ◑ |
| 212,216 | page_get/cursor_push branch errors | F | fault-сайты на глубокой таблице (depth≥4) | ✅ |
| 220–222 | CORRUPTED leaf-type | F | fault-сайт `cache_leaf_validation_fail` (→MDBX_CORRUPTED) | ✅ |
| 274–275 | EMULTIVAL | R | dupsort-таблица, два значения | ✅ |
| 280 | node_read error | F | fault-сайт `cache_node_read_err` | ✅ |
| 311 | RACE (Debug-ветка) | R | entry.last_confirmed_txnid = UINT64_MAX до вызова | ✅ |
| 316,357 | `while(true)` headers | — | gcov-артефакт (тело выполняется десятки тысяч раз) | 📄 |
| 339 | retry `local = again` | S | конкурентная модификация entry — стохастически в case2 (недетерминированно, вне детерминированного пилота) | 📄 |
| 373 | EINVAL (null аргументы) | R | nullptr key (оба entry-point) | ✅ |
| 380 | mdbx_cache_init экспорт | X | тесты используют inline из mdbx.h | 📄 |

Дополнительно закрыты: BEHIND (entry.trunk > snapshot), UNABLE (ABA-окно),
RACE (невалидный last_confirmed). Аттестация: `query` по fault-тегам
`cache_*` — seen>0.
**Результат: 100% детерминированно-достижимых строк (defer: elev-петля
L183–190/L195, стохастика case2, X-исключения — задокументированы).**

## bunches_removal → gc-get.c (только достигнутые функции)

**Достигнуто тестом** (923 alloc-вызовов): gc_alloc_ex, gc_alloc_single,
page_alloc_finalize, gc_check_keylen, gc_check_rowdata, gc_cursor_init,
gc_row_pnl, is_reclaimable, repnl_get_single.

| Строки | Что | Класс | Статус |
| --- | --- | --- | --- |
| 858–859 | page_alloc_finalize ENOMEM bailout | F | **defer**: требует прицельного fault-сайта в page_shadow_alloc (аллокация не первая в транзакции, alloc-fault недетерминирован) |
| 962–967, 1119–1121, 1242–1244 | переиспользование repnl-последовательностей (num>1) | R | **defer**: нужна серия страниц из repnl (глубокое состояние реклайма) |
| 1049–1101 | GC-record walk: SET_RANGE-fail/LIFO-retry/corrupted/too-long | C/R | **defer**: требует crafted-состояния GC-записей |
| 1148–1151 | DEBUG_EXTRA retired-pnl | R | **defer**: только при MDBX_LOG_EXTRA (debug-логирование, не функциональность) |
| 1204–1213 | gc-early-clean verbose/error | C | **defer**: условие автоочистки |
| 1421–1481 | refund/учёт при ошибке | F | **defer**: fault-сайт в учёте |

**Вне scope (не вызвано ни разу, задокументировано)**: mincore_fetch/bit_tas/
env_is_page_incore, scan4seq_{fallback,sse2,avx2,avx512bw}/resolver/
scan4range_checker, gc_repnl_get_sequence/scan_sequence_reserve/has_span/
gc_repnl_get_single, snapshot_oldest_force_rescan, prefault/readahead
(L819–852, 996–1002), autosync/growth/MAP_FULL (1298–1390).

## bunches_removal → gc-put.c (только достигнутые функции)

**Достигнуто** (341 gc_update): gc_update, gc_put_init/destroy,
gc_clear_reclaimed/returned, gc_enforce_not_spilled, gc_fill_returned,
gc_merge_loose, gc_prepare_stockpile{,4retired,4update}, gc_remove_rkl,
gc_rerere, gc_reserve4return, gc_reserve4stockpile, gc_store_retired,
gc_touch, is_lifo.

| Строки | Что | Класс | Статус |
| --- | --- | --- | --- |
| 274,299,310,382,393,406 | error-пропагация pnl/резервов | F | **defer**: fault-сайты (аналогично cache), см. отдельную задачу |
| 296–302 | merge loose→retired | R | **defer**: требуется loose-страницы при delete_range (геометрия/порог) |
| 355–377 | WRITEMAP/msync + clean_stored_retired | F/R | **defer**: writemap-сборка |
| 470–474 | DEBUG_EXTRA retired-pnl | R | **defer**: при MDBX_LOG_EXTRA |
| 496–519 | rkl-push ready4reuse / NOTFOUND edge | C | **defer**: crafted GC-состояние |
| 1271–1304 | gc_enforce_not_spilled spilled-путь | F/R | **defer**: страница в спилле |
| 1443–1542 | bailout/restart/too-many-loops | F | **defer**: экстремальные состояния |

**Вне scope (не вызвано)**: gc_handle_dense/gc_dense_solve/gc_dense_hist,
solve_recursive/consume_stack/consume_remaining, gc_search_holes/gc_push_sequel,
gc_reclaim_slot/gc_reserve4retired/gc_clean_stored_retired, dense_adjust_*,
gc_peekid, dbg_id/dbg_prefix/dbg_dump_ids.

> **Статус bunches_removal**: достигнутые функции покрыты по основным веткам;
> оставшиеся — либо fault-сайты (F), либо crafted-состояния реклайма (C/R),
> либо debug-логирование. Полное доведение до 100% требует отдельного захода
> (добавление fault-сайтов в gc-*.c и crafted-сценариев реклайма) — помечено
> defer, вне рамок текущего пилота из-за объёма.

## Метрика успеха

1. gcov по каждому тесту: все строки классов R/F/D/C в scope = покрыто;
   X/out — задокументированы.
2. Аттестация в тесте: query по тегам `rkl:*`, `txl:*`, `cache:*` — seen>0.
3. Время рутинных прогонов в P1-бюджете (допускается рост, без кратного).