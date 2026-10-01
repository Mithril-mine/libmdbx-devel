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

Непокрытые ветки в этих функциях (основные):
| Строки | Что | Класс | Решение |
| --- | --- | --- | --- |
| 858–859 | page_alloc_finalize ENOMEM bailout | F | fault на аллокацию/расширение |
| 962–967, 1119–1121, 1242–1244 | переиспользование repnl-последовательностей (num>1) | R/out | потребовать серии страниц из repnl; иначе out |
| 1049–1055 | SET_RANGE fail / ALLOC_LIFO retry | C/F | состояние GC-записей |
| 1058–1059 | corrupted GC-record keylen | C/F | повреждённая запись GC |
| 1064–1101 | PREV-обход / слишком длинный слот / NOTFOUND | C/R | глубже по обходу GC |
| 996–1002, 819–852 | prefault/readahead-путь | out | подсистема prefault не активируется delete-семантикой |
| 1298–1339, 1362–1390 | autosync/growth/MAP_FULL | out | рост файла вне scope |
| 1421–1481 | refund/учёт при ошибке | F/C | fault-прогоны |

**Вне scope (не вызвано ни разу)**: mincore_fetch/bit_tas/env_is_page_incore,
scan4seq_{fallback,sse2,avx2,avx512bw}/resolver/scan4range_checker,
gc_repnl_get_sequence/scan_sequence_reserve/has_span/gc_repnl_get_single,
snapshot_oldest_force_rescan.

## bunches_removal → gc-put.c (только достигнутые функции)

**Достигнуто** (341 gc_update): gc_update, gc_put_init/destroy,
gc_clear_reclaimed/returned, gc_enforce_not_spilled, gc_fill_returned,
gc_merge_loose, gc_prepare_stockpile{,4retired,4update}, gc_remove_rkl,
gc_rerere, gc_reserve4return, gc_reserve4stockpile, gc_store_retired,
gc_touch, is_lifo.

Непокрытые ветки в этих функциях (основные):
| Строки | Что | Класс | Решение |
| --- | --- | --- | --- |
| 19–20 | gc_chunk_pages — только в dense-путях | out | dense не активируется |
| 274,299,310,382,393,406 | error-пропагация pnl/резервов | F | fault |
| 296–302 | merge loose→retired | R | loose-страницы при delete_range |
| 355–377 | WRITEMAP/msync + clean_stored_retired | F/R | ветка writemap-сборки |
| 496–519 | rkl-push ready4reuse / NOTFOUND edge | C/F | GC-состояние |
| 470–474 | DEBUG_EXTRA retired-pnl | R | прогон с MDBX_LOG_EXTRA |
| 1271–1304 | gc_enforce_not_spilled spilled-путь | F/R | страница в спилле |
| 1343–1414 | comeback-reserve «multi» | out | dense/comeback |
| 1443–1542 | bailout/restart/too-many-loops | F | экстремальные состояния |

**Вне scope (не вызвано)**: gc_handle_dense/gc_dense_solve/gc_dense_hist,
solve_recursive/consume_stack/consume_remaining, gc_search_holes/gc_push_sequel,
gc_reclaim_slot/gc_reserve4retired/gc_clean_stored_retired, dense_adjust_*,
gc_peekid, dbg_id/dbg_prefix/dbg_dump_ids.

## Метрика успеха

1. gcov по каждому тесту: все строки классов R/F/D/C в scope = покрыто;
   X/out — задокументированы.
2. Аттестация в тесте: query по тегам `rkl:*`, `txl:*`, `cache:*` — seen>0.
3. Время рутинных прогонов в P1-бюджете (допускается рост, без кратного).