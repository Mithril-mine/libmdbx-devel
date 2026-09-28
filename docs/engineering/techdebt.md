# Tech Debt Inventory (src/)

> Part of the [Skynet project index](README.md).
> Automated scan for `TODO|FIXME|XXX|workaround|hack|temporary` across `src/*.c` (and one hit in
> `mdbx++`): **86 markers**. Grouped by category; file:line references are exact at scan time.
> Purpose: prioritized candidates for refactoring. Re-scan with
> `grep -rnE 'TODO|FIXME|XXX|workaround|hack|temporary' src/` to refresh.

Legend: 🔴 must-fix/blocking-adjacent · 🟠 improvement opportunity · ⚪ informational/
hardware-platform edge · 🧹 removal candidate (dead/obsolete branch).

---

## 1. Платформенные workaround'ы (важно понимать при рефакторинге портируемости)

| # | Маркер | Место | Суть |
| --- | --- | --- | --- |
| 🟠 | workaround (incoherent unified page/buffer cache) | `coherency.c:25,32,41,49,63,78,96-99,108,151`; `dxb.c:1316`; `page-iov.c:38,95`; `txn-basal.c:143`; `api-env.c:1206` | Кластер обработки **issue #269** — несогласованность unified page cache; циклы-ожидания прихода страниц, stuck_meta, «wagering meta». Самый разветвлённый workaround в кодовой базе; при рефакторинге путей записи/синка трогать аккуратно |
| 🟠 | `workaround_glibc_bug21031` | `rthc.c:191,555` | TLS-destructor баг glibc #21031; вызывается перед регистрацией читателей; связан с инвариантом TLS-очистки (см. architecture.md §8.6) |
| 🟠 | workaround Win10 UCRT bug | `lck-windows.c:204,288` | `GetExitCodeThread`/`ERROR_ACCESS_DENIED` при проверке живости PID читателя |
| 🟠 | workaround for Wine | `osal.c:1994,2488` | `running_under_Wine`; запрет эксклюзива / расширения rw-секции под Wine |
| 🟠 | temporary workaround OpenBSD kernel flaw (issue #67) | `api-env.c:452`; `tools/chk.c:479` | `MDBX_MMAP_INCOHERENT_FILE_WRITE` ветка |
| ⚪ | workaround musl libc wrong prototype | `osal.c:159-160` | POSIX-прототип для musl vs glibc |
| ⚪ | workaround ecryptfs bug(s) | `api-copy.c:590` | EXDEV при копировании между ФС |
| ⚪ | workaround «private field not used» (LCC) | `src/mdbx.c++:171-175` (класс `bug`) | `#ifndef __LCC__` для Elbrus |

## 2. Реализационные TODO (потенциальные улучшения)

| # | Маркер | Место | Суть | Рефакторинг-потенциал |
| --- | --- | --- | --- | --- |
| 🟠 | io_uring TODO | `osal.c:1095-1109` | Заглушка асинхронного write-пути (io_uring_prep_write/submit/wait закомментированы) | Высокий: асинхронный I/O-бэкенд |
| 🟠 | TODO кэш пропускаемых последовательностей GC | `gc-get.c:633` | Сканирование списка с начала при каждом выделении | Средний (перф GC) |
| 🟠 | TODO критерии `gc_provide_slots()` | `gc-get.c:1091-1093` | Упрощённая проверка при нехватке идентификаторов | Средний |
| 🟠 | TODO двусвязный список курсоров | `api-cursor.c:90-92` | `cursor->backup` (курсор родителя во вложенной txn) возвращает MDBX_EINVAL до реализации | Средний (расширение API) |
| 🟠 | TODO headroom для rkl | `rkl.c:184-186` | Избавиться от memmove через list_begin/list_end/list_buffer | Низкий |
| 🟠 | TODO extended info для dbi | `dbi.c:267,277` | `return /* FIXME: return extended info */ MDBX_INCOMPATIBLE` при конфликте флагов | Низкий |
| 🟠 | TODO внешняя аллокация курсоров | `api-txn-data.c:329-331` | Замещение внутренних курсоров пользовательскими (без malloc) | Средний (перф/API) |
| 🟠 | TODO range-sync flush_begin/end | `txn-basal.c:440` | Использовать ctx.flush_begin/flush_end для range-sync | Низкий |
| 🟠 | TODO размер порции defrag | `defrag.c:1168-1171` | Динамическая подстройка времени записи порции к таймауту | Средний |
| 🟠 | TODO use md_subs | `table.c:211` | Поле `md_subs` вместо подсчёта items | Низкий |
| 🟠 | TODO suitable4loose | `page-ops.c:376-378` | Экономия последовательностей при выборе loose-страниц | Средний |
| 🟠 | TODO multivalues для get-cached | `api-get-cached.c:272-274` | Сейчас MDBX_EMULTIVAL для N_DUP | Средний (расширение фичи) |
| 🟠 | TODO use options? factor | `api-opts.c:181` | Жёсткий фактор 9 при расчёте | Низкий |
| 🟠 | TODO step-by-step allocation | `osal.c:1793-1794` | F_PREALLOCATE чанками 16K/8K/4K/2K/1K | Низкий |
| 🟠 | TODO suspend/resume threads | `osal.c:2762-2764` | unmap/map при ресайзе на POSIX | Средний |
| 🟠 | TODO avoid search in page_unspill | `cursor.c:1109` | Избежать повторного поиска при unspill | Низкий |
| 🟠 | TODO min/max keylen от пользователя | `cursor.c:1306-1307` | Опция ограничения размеров ключей/данных | Средний (новая фича) |
| 🟠 | TODO slice operator<< | `mdbx++/decl_slice.h++:15` | Чтение через operator<< | Низкий (C++ API) |

## 3. FIXME «Not implemented» / упразднённые альтернативы

| # | Маркер | Место | Суть | Рефакторинг-потенциал |
| --- | --- | --- | --- | --- |
| 🧹 | `MDBX_PNL_ASCENDING=0` «no longer supported since 2026-04-01» | `pnl.c:282,316`; `gc-get.c:239,310,376,451`; `defrag.c:699` | Альтернативный порядок PNL упразднён — остались `#error` в неактивных ветках | **Высокий (очистка)**: ветки можно удалить, упростив pnl/gc/defrag |
| 🧹 | byte-order FIXME | `global.c:112,454`; `rthc.c:114,143`; `osal.c:292` | `#error "FIXME: Unsupported byte order"` в неактивных ветках | Низкий (edge) |
| 🧹 | locking variants FIXME | `lck-posix.c:211,221,588,670,734,791,814` | `#error FIXME` в неподдерживаемых комбинациях `MDBX_LOCKING`; `#pragma message/#warning TODO` (728-731); «Not implemented» для одного из вариантов (588) | Средний (при работе с locking) |
| 🧹 | `#warning "FIXME"` MADV | `dxb.c:472,505,525` | Неопределённые `MADV_RANDOM`/аналоги | Низкий |
| 🧹 | FIXME TODO clang/MSVC | `osal.c:519,591` | Ветки без реализации для отдельных компиляторов | Низкий |
| 🧹 | FIXME RAM query | `osal.c:3600,3650` | `#error "FIXME: Get Available RAM"` для неизвестных платформ | Низкий (edge) |
| ⚪ | FIXME Optional page faults | `osal.c:3003` | Неопределённая ветка | Низкий |
| 🟠 | FIXME реставрация БД | `dxb.c:134-135` | TODO: полная проверка B-tree при повреждении мета | Средний (фича chk) |
| 🟠 | FIXME mod_txnid семантика | `api-misc.c:78-79` | Итоговое решение по установке/обновлению mod_txnid | Средний (инвариант) |
| 🟠 | FIXME re-check size DB-file | `env.c:420` | Перепроверять размер файла в env_open | Низкий |
| 🟠 | TODO:FIXME page_split | `page-split.c:93` | Недостижимая ветка при невозможности split влево | Низкий |

## 4. Прочие заметки (контекст, не дефекты)

- `logging_and_debug.c:342-344` — комментарий о том, почему page-структуры не проверяются
  во время модификации дерева (важный контекст для chk/валидации).
- `tools/dump.c:440-442` — hack rescue-режима mdbx_dump (не «делать так» — перезапуск txn
  при ошибке).
- `stochastic.sh`, `battery-tmux.sh` — маркеры «FIXME: Fake support» для MSYS/MINGW в
  скриптах (см. build.md §6.2).

## 5. Рекомендации

1. **Первая волна (дешёвая, безопасная):** удаление упразднённых веток
   `MDBX_PNL_ASCENDING=0` (6 мест) и byte-order/`#warning` FIXME — упростит pnl/gc/defrag без
   изменения поведения (уровни верификации 1–2 + тест `details_rkl` и `dupfix_*`).
2. **Вторая волна (средняя):** кластер #269 (coherency/dxb/page-iov) — перед любым
   рефакторингом путей записи; io_uring-заглушка в osal — как отдельный проект.
3. **Перед каждым рефакторингом конкретного модуля** сверяться с этим списком: техдолг
   модуля может определить границы изменений (например, locking-варианты при работе с
   `lck-posix.c`).
4. Обновлять этот документ после крупных рефакторингов (маркеры убывают).