# Deep dive: блокировки, RLT, HSR, fork, долговечность и восстановление

> Часть индекса [документации разработчика](README.md).
> Дополняет [`functional-architecture.md`](functional-architecture.md) и
> [`architecture.md`](architecture.md) по темам, которые там даны кратко:
> файл блокировок (LCK) и его форматы, таблица читателей (RLT), HSR-протокол,
> поведение после `fork()`, WRITEMAP/авто-синхронизация и режимы восстановления.
> Все факты — из кода `master@f957a778`; источники указаны как `file:line`.

---

## 1. Файл блокировок (LCK): layout и версии

- Отдельный от данных файл блокировок рядом с файлом БД. Расположен в общем mmap
  (`shared_lck`, `src/layout-lck.h:189`).
- Сигнатура/версия файла блокировок **зависит от flavour'а блокировок** (`MDBX_LOCKING`):
  `MDBX_LCK_SIGN` = `0xF10C` (WIN32FILES), `0xF18D` (SYSV), `0x8017` (POSIX2001/2008),
  `0xFC29` (POSIX1988) — `src/layout-lck.h:12–28`. Контрольная сумма LCK включает
  `sizeof(reader_slot_t)` — `src/layout-lck.h:288`.
- Flavour'ы задаются в `src/options.h:337–349`: `MDBX_LOCKING_WIN32FILES=-1`,
  `MDBX_LOCKING_SYSV=5`, `MDBX_LOCKING_POSIX1988=1988`, `MDBX_LOCKING_POSIX2001=2001`,
  `MDBX_LOCKING_POSIX2008=2008`.
- Формат LCK **версионирован отдельно** от формата данных: изменение структуры
  `shared_lck`/`reader_slot_t` ломает только совместный доступ к уже открытой базе,
  но не сам файл данных.

## 2. Таблица читателей (RLT) и слота читателя

Структура слота (`src/layout-lck.h:147–180`):

| Поле | Смысл |
| --- | --- |
| `txnid` (atomic) | Номер снапшота, с которого читатель начал (или `INVALID_TXNID`) |
| `tid` | Thread ID владельца слота; псевдо-значения: `MDBX_TID_TXN_PARKED = UINT64_MAX`, `MDBX_TID_TXN_OUSTED = UINT64_MAX-1` (`layout-lck.h:163–168`) |
| `pid` | Process ID владельца |
| `snapshot_pages_used` | `first_unallocated` на момент снапшота (сколько страниц читатель «пинит») |
| `snapshot_pages_retired` | Число retired-страниц на момент старта; разность `meta.pages_retired − reader.snapshot_pages_retired` = сколько страниц этот читатель удерживает от переиспользования |

Комментарий в коде честно отмечает: **stale-слоты сейчас не проверяются** — таблица
переинициализируется целиком, когда известно, что мы единственный процесс,
открывающий LCK (`src/layout-lck.h:158–162`). Очистка «мёртвых» читателей идёт
через механизмы живости (см. ниже).

## 3. Механизмы живости и повторное использование pid/tid

- Живость потоков/процессов проверяется средствами ОС; защита от повторного
  использования pid/tid предотвращает ложное «оживление» слота умершего потока
  новым потоком с тем же идентификатором.
- При **парковке** (`mdbx_txn_park`) слот помечается псевдо-`tid = MDBX_TID_TXN_PARKED` —
  перестаёт влиять на детент; при **вытеснении** — `MDBX_TID_TXN_OUSTED` (CAS-переход
  паркером). Детали состояний — `functional-architecture.md` §14.9.
- Очистка «зависших» читателей выполняется при открытии и перед ростом БД;
  авто-очистка «зависших» писателей — по признаку устаревшей регистрации.

## 4. HSR-протокол (Handle-Slow-Readers)

Полная сигнатура колбэка (`mdbx.h:7014`):

```c
typedef int (*MDBX_hsr_func)(const MDBX_env *env, const MDBX_txn *txn,
                             mdbx_pid_t pid, mdbx_tid_t tid,
                             uint64_t laggard, unsigned gap,
                             size_t space, int retry) MDBX_CXX17_NOEXCEPT;
```

Параметры:
- `laggard` — отставание проблемного читателя (снапшот отстаёт от свежего коммита);
- `gap` — число повторов/попыток;
- `space` — объём пространства, который освободится после завершения читателя;
- `retry` — счётчик повторных вызовов.

Возвращаемые значения (по doxygen, `mdbx.h:7001–7012`):
- `0` — колбэк решил проблему или просто подождал; libmdbx пересканирует таблицу
  читателей и повторяет попытку. Включает случай, когда проблемная транзакция
  завершилась нормально (`abort`/`reset`) — чистить слот не нужно.
- `1` — транзакция-читатель прервана асинхронно, слот следует очистить немедленно
  (ни `mdbx_txn_abort()`, ни `mdbx_txn_reset()` уже вызваны не будут).
- `2+` — процесс-читатель завершён/убит: libmdbx полностью сбрасывает его регистрацию.

Установка: `mdbx_env_set_hsr()` (`mdbx.h:7035`); получение — `mdbx_env_get_hsr()`.
Вызывается **только** когда база заполнена из-за читателей, блокирующих
переиспользование страниц.

## 5. Поведение после fork()

- После `fork()` наследник не наследует mmap- и record-блокировки. Использовать
  окружение в дочернем процессе можно только после `mdbx_env_resurrect_after_fork()`
  (`mdbx.h:3214–3252`), которая переоткрывает/восстанавливает перенесённый экземпляр
  среды (PID сменился, регистрации сброшены).
- Запрет повторного открытия одной БД в пределах процесса защищает от гонок;
  legacy-режим — `MDBX_DBG_LEGACY_MULTIOPEN` (`mdbx.h:972`; включается также
  переменной окружения `MDBX_DBG_LEGACY_MULTIOPEN`, `src/global.c:283`; проверка
  в `src/lck-posix.c:73`).

## 6. WRITEMAP и связанные опции

- `MDBX_WRITEMAP` — запись через mmap (+`msync` вместо `pwrite`).
- **Prefault-запись**: `MDBX_opt_prefault_write_enable` (`mdbx.h:2362`) — упреждающая
  запись страниц, чтобы устранить page-fault'ы и чтения с диска при первом обращении
  в WRITEMAP-режиме.
- **mincore-отслеживание** резидентности страниц используется для предотвращения
  page-fault'ов в WRITEMAP.
- **Writethrough-порог**: `MDBX_opt_writethrough_threshold` (`mdbx.h:2357`) — выбор
  между сквозной записью (`O_DSYNC`) и записью с последующим `fdatasync()`;
  влияет только на `MDBX_SYNC_DURABLE` (`mdbx.h:2353`).
- **Некогерентность unified page cache**: защита `MDBX_FORCE_CHECK_MMAP_COHERENCY`
  (`src/page-iov.c:54,85–87`; включение — опция сборки, дефолт 0) — workaround
  issue #269, внесён в сериях 0.11.5–0.11.6.

## 7. Автоматическая синхронизация

- `mdbx_env_set_syncbytes()` / `mdbx_env_set_syncperiod()` (`mdbx.h:1452,2188,2194`)
  + опции `MDBX_opt_sync_bytes` / `MDBX_opt_sync_period` — авто-sync по объёму
  записанного и/или по таймауту; текущие пороги видны в `mdbx_env_info_ex`
  (`mdbx.h:2926,2933`).
- `MDBX_opt_presync_threshold` (`mdbx.h:2484`) — порог предварительной
  подготовки флаша; асинхронные `mdbx_env_sync_ex()` / `mdbx_env_sync_poll()`
  (`mdbx.h:2469,2478–2479,3011`) с предварительной проверкой и повторной попыткой.

## 8. Восстановление и диагностические коды

- `mdbx_env_open_for_recovery()` (`mdbx.h:7079`; Windows-вариант `...W`:
  `mdbx.h:7086`) — открытие БД с выбором целевой мета-страницы (`target_meta`)
  и флагом `writeable`; начиная с 0.12.7 recovery-проверки **не изменяют базу**.
- Код `MDBX_WANNA_RECOVERY = -30419` (`mdbx.h:1987`) возвращается, когда задан
  `MDBX_RDONLY`, но БД требует процедуры восстановления (`mdbx.h:2583`).
- Код `MDBX_MVCC_RETARDED = -30410` (`mdbx.h:2020`) — читатель старше актуального
  MVCC-снапшота (см. `mdbx.h:1735`).
- Управление steady-point'ами и автоматический steady при нехватке пространства —
  `functional-architecture.md` §8.

## 9. Что осталось вне этого документа

- Точные алгоритмы split/merge/rebalance, spill-LRU и SIMD-ядер — см.
  [`functional-architecture.md`](functional-architecture.md) §7, §11 и §14.
- Профилирование GC (`MDBX_ENABLE_PROFGC`) и чтение `commit_latency` —
  [`debugging-methodology.md`](debugging-methodology.md).
- Полный каталог доработок над LMDB — [`libmdbx-improvements.md`](libmdbx-improvements.md).