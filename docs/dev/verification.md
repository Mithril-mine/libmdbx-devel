# Verification log — docs/dev vs master@f957a778

> Сверка перенесённых документов с актуальным кодом `master@f957a778`
> (2026-09-29). Правило: каждый факт — из кода; расхождения исправлены в тексте.

## Документы

| Документ | Статус | Примечания |
| --- | --- | --- |
| `architecture.md` | ✅ сверен | ВСЕ утверждения подтверждены. **Исправлено:** GC-рециклинг — FIFO по умолчанию, LIFO только при `MDBX_LIFORECLAIM` (`src/gc-get.c:980`); ранее в тексте ошибочно «LIFO by default». |
| `functional-architecture.md` | ✅ сверен | Разделы 2–14 подтверждены кодом; численные ориентиры совпадают. |
| `functional-architecture.en.md` | ✅ сверен | То же, что RU-версия. |
| `libmdbx-improvements.md` | ✅ сверен (выборочно) | API/механизмы/константы подтверждены; версии-ориентиры §15 из ChangeLog. |
| `libmdbx-improvements.en.md` | ✅ сверен (выборочно) | То же, что RU-версия. |
| `module-interfaces.md` | ✅ сверен | Все модули/заголовки/прототипы существуют; include backbone совпадает с `essentials.h`/`internals.h`. |
| `deep-dive.md` | ✅ создан по коду | Дополняет пробелы покрытия инструкции (LCK/RLT/HSR/fork/WRITEMAP/recovery), все факты из `master@f957a778`. |
| `debugging-methodology.md` | ✅ создан по коду | Полный документ по §4 инструкции: инструменты (`mdbx_chk` man, PROFGC-поля из `layout-lck.h`, `MDBX_commit_latency` из `mdbx.h`, `MDBX_CHECKING/DEBUG` из `options.h`, санитайзеры из `profile.cmake`), 12 сценариев, внутренняя отладка. |
| `structure-review.md` | ⚠️ частично | Аудит описывает полную структуру старого `devel` (ныне `poc1-failed`). На master после переноса: `.codeassistant/` отсутствует, `tests/`=79 (было 112), `skynet/`=57 (было 67), `src/`=119 (совпадает). Выводы F1–F12 и решения C1–C9 сохраняют силу. Пометка добавлена в шапку. |

## Проверенные факты (выборка, для воспроизводимости)

### architecture.md
- `MDBX_DATA_VERSION = 3` — `src/layout-dxb.h:17`
- `troika_t` — `src/internals.h:36`, `MDBX_txn.troika` — `src/internals.h:241`
- `z_eof_soft/z_eof_hard` — `src/cursor.h:33–43`
- `ALLOC_LIFO` включается флагом `MDBX_LIFORECLAIM` — `src/gc-get.c:979–980`
- `mvcc_bind_slot` — `src/proto.h:17`; `txn_setup_primal` — `proto.h:89`; `txn_ro_start` — `proto.h:111`
- `env_owned_wrtxn` — `src/env.c:6`, `proto.h:120`
- `gc_reclaiming_obstacle` — `src/gc.h:8`
- предикаты `is_frozen/spilled/shadowed/modifiable/tmp` — `src/page-ops.h:14–52`
- двухфазная мета: `txnid_b` ноль/запись — `src/meta.c`
- `MDBX_MAGIC = 0x59659DBDEF4C11` — `src/layout-dxb.h:14`
- `dp_limit` авто-настройка `(total_ram + avail_ram)/42` — `src/api-opts.c:11–33`

### functional-architecture.md
- `NUM_METAS = 3` — `src/layout-dxb.h:31`
- тройка: `troika_fsm_map[2*2*2*3*3*3]` (216) — `src/meta.c:73`
- `MAX_GC1OVPAGE(pagesize)` — `src/cogs.h:59`
- `MDBX_MAX_DBI = 32765` — `mdbx.h:819`
- `MDBX_MAXDATASIZE = 0x7fff0000` (≈2 ГБ) — `mdbx.h:822`
- get-cached статусы `MDBX_CACHE_{HIT,CONFIRMED,REFRESHED,DIRTY,BEHIND,UNABLE,RACE}` — `mdbx.h:5332–5387`
- `txn_space_retired` / `txn_space_leftover` — `mdbx.h:4119,4126`
- `mdbx_estimate_distance/move/range` — `mdbx.h:6683,6705,6732`
- `mdbx_cursor_get_batch` — `mdbx.h:6231`; `mdbx_cursor_bunch_delete` — `mdbx.h:6366` (see)
- `mdbx_key_from_double` — `mdbx.h:5040`; `mdbx_dbi_dupsort_depthmask` — `mdbx.h:5104`
- WRITEMAP несовместим с nested — `src/txn-nested.c:437–438`
- `prefer_waf_insteadof_balance` / `merge_threshold` — `mdbx.h:2328–2329`
- `MDBX_LIFORECLAIM = 0x4000000` — `mdbx.h:1329` (включает LIFO)

### libmdbx-improvements.md
- `MDBX_opt_*` набор — `mdbx.h` (33 упоминания)
- `scan4seq_*` SIMD-ядра — `src/gc-get.c` (31)
- `merge_threshold` дефолт 33% (`65536/3`) — `src/api-opts.c:79–82`
- early GC cleanup — `src/gc-get.c:1203` («gc-early-clean»)
- `rp_augment_limit`/`gc_time_limit` — `src/api-opts.c:37,301`

### structure-review.md
- Расхождения счётчиков на master: `src/`=119 ✅, `tests/`=79 (док: 112),
  `skynet/`=57 (док: 67), `.codeassistant/` отсутствует (док: 44+1)
- `src/chk.c` (библиотека, `mdbx_env_chk`) vs `src/tools/chk.c` (CLI) — разделение
  сохранено на master