# Пробники USDT/DTrace в libmdbx

> Каталог и описание статических трассировочных пробников (`USDT`/`SystemTap`/`DTrace`),
> встроенных в движок. Наполняется в рамках **TASK-14** (`B3` бэклога).
> Терминология и сценарии использования — в `skynet/test-scenarios.md` (§2, §3).

## 1. Зачем

Статические пробники дают тестам и эксплуатации три возможности (см. test-scenarios §2.2):

1. **Наблюдать путь исполнения** — на какие ветви попало выполнение
   (страница из GC vs хвост файла vs рост файла; включился ли спилл; ...).
2. **Читать значения** локальных переменных/счётчиков на границах фаз.
3. **Инъектировать ошибки** — SystemTap `return`-пробник может переписать возвращаемое
   значение (эмуляция сбоя), см. §5.

## 2. Соглашение об именах

Согласовано с главным архитектором (ANSWERED, `mail-2-main_architect-sysprobe`):

```
mdbx::<подсистема>::<фаза>::<событие>
```

Отображение в USDT-имя (SystemTap/DTrace): `::` → `__`, провайдер всегда `mdbx`.

| Логическое имя | USDT-маркер |
| --- | --- |
| `mdbx::alloc::source` | `mdbx:alloc__source` |
| `mdbx::commit::gc_update::begin` | `mdbx:commit__gc_update__begin` |
| `mdbx::commit::gc_update::end` | `mdbx:commit__gc_update__end` |
| `mdbx::spill::trigger` | `mdbx:spill__trigger` |
| `mdbx::txn::write_started` | `mdbx:txn__write_started` |
| `mdbx::panic` | `mdbx:panic` (существовал до TASK-14) |

Свойства «болевой точки» (test-scenarios §2.3): граница фаз (вход/выход), параметры —
простые типы (`txnid`, `pgno`, счётчики, коды), для инъекции — точка после принятия решения.

## 3. Как включить

- CMake: `cmake -DENABLE_SYSTEMTAP=ON` (требует `sys/sdt.h`; проверка в
  `cmake/profile.cmake`). Опция `ENABLE_DTRACE=ON` эквивалентна на платформах
  с `<sys/sdt.h>` (Linux, macOS, *BSD, Solaris).
- GNUmakefile: цель `cmake-probes-build` собирает с `-DENABLE_SYSTEMTAP=ON` и
  выводит число маркеров.
- Без опций пробники компилируются в `__noop` (ноль-стоимость, без `.note.stapsdt`).

## 4. Каталог пробников

| Маркер | Модуль/функция | Аргументы | Смысл |
| --- | --- | --- | --- |
| `mdbx:alloc__source` | `gc-get.c` → `gc_alloc_ex()` (после выбора страницы) | `pgno` (u64), `mode` (u32) | Источник страницы: `mode=0` — из GC (reclaimed/loose), `mode=1` — хвост неразмеченного пространства (tail), `mode=2` — файл вырос (`dxb_resize`). `pgno` — номер первой страницы. Позволяет тесту отличить «переиспользование» от «роста» (сценарий SC-1). |
| `mdbx:commit__gc_update__begin` | `gc-put.c` → `gc_update()` (вход) | `txnid` (u64), `loop` (u32) | Начало цикла обновления GC-дерева при коммите; `loop` — номер итерации (0 первая). |
| `mdbx:commit__gc_update__end` | `gc-put.c` → `gc_update()` (выход) | `txnid` (u64), `rc` (i32) | Конец обновления GC; `rc` — код возврата (0 = успех). Готовая точка инъекции: перезапись `rc`. |
| `mdbx:spill__trigger` | `spill.c` → `spill_slowpath()` (перед ветвлением WRITEMAP/non) | `need_spill` (u64), `dirty_entries` (u64), `dirty_npages` (u64) | Сработал порог спилла; сколько нужно освободить и сколько грязных записей/страниц есть. |
| `mdbx:txn__write_started` | `txn-basal.c` → `basal_start_locked()` (после захвата) | `txnid` (u64), `front_txnid` (u64) | Пишущая транзакция стартовала (захвачен wrt_lock, выбран снапшот и расчётный id). `front_txnid` — «призрачный» id незакоммиченных страниц. |
| `mdbx:panic` | `logging_and_debug.c` → `panic_internal()` | `func` (ptr), `line` (u32), `msg` (ptr), `obj_class` (ptr), `obj` (ptr) | Аварийная остановка движка. |

## 5. Инъекция ошибок (fault injection)

SystemTap позволяет переписывать возвращаемое значение пробника, стоящего «на выходе»:

```systemtap
# эмуляция исчерпания карты на mdbx:commit__gc_update__end (rc) не нужна —
# это внутренний int; для MAP_FULL эмулируем на аллокаторе:
probe process("/path/to/libmdbx.so").mark("alloc__source") {
  if (user_int($arg2) == 0 && randint(100) < 5) { $arg2 = 2 }  # заставить расти
}
```

Полноценная эмуляция `MDBX_MAP_FULL` и других кодов — через `return`-пробники
(точки выхода функций, где решение уже принято). Требуется рантайм SystemTap
(`stap`), которого нет на сборочных хостах CI; для CI-проверки присутствия
пробников используется `tests/probes-check.sh` (см. §6).

## 6. Проверка без рантайма SystemTap

- `readelf -n <libmdbx.so>` показывает секцию `.note.stapsdt` со всеми маркерами.
- `tests/probes-check.sh <elffile>` — проверяет наличие ожидаемого набора
  маркеров, пригоден для CI (не требует root и `stap`).

## 7. Кандидаты следующих партий (test-scenarios §2.4)

- `mdbx::reader::snapshot_begin/end` (txnid, число ретраев) — снятие снапшота читателя.
- `mdbx::commit::refund` (кол-во возвращённых страниц) — сработал refund.
- `mdbx::meta::two_phase::validate` (txnid_a/b, steady/weak) — двухфазное обновление меты.
- `mdbx::gc::detent_update` (oldest_txnid) — детент двинулся (SC-1).
- `mdbx::reader::slot_release` (tid) — освобождение слота читателя (SC-1).
- `mdbx::recovery::meta_selected` (мета, stuck_meta) — открытие/восстановление.
- `mdbx::bigfoot::chain` (длина цепочки, эффективный txnid) — bigfoot-цепочки (SC-2).
- `mdbx::hsr::kick` (pid, lag, space_retired) — отложенные читатели.