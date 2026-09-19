# Доработки libmdbx относительно LMDB: каталог

> Часть индекса [Skynet](README.md).
> Систематический перечень того, что _libmdbx_ (глубоко переработанный потомок LMDB, форк 2015 г.)
> добавил, изменил и улучшил по сравнению с LMDB: возможности, функции API, механизмы,
> исправления и оптимизации — вплоть до микрооптимизаций (SIMD, branchless-поиск, атомики и т.п.),
> **без перечисления отдельных мест в коде и точечных правок**.
>
> Источники: текущий и исторический `README.md`, документация в `docs/`, `ChangeLog*.md`
> всех серий (0.09–0.14), полный список runtime-опций (`MDBX_opt_*`). Версии указаны как
> ориентиры «впервые появилось/стало заметным»; часть функциональности развивалась постепенно.

---

## 1. Сводная карта

| Категория | Краткая суть | Ключевые пункты |
| --- | --- | --- |
| A. Надёжность | Устранены дефекты, до сих пор живущие в LMDB | ~§2 |
| B. Модель данных и формат | Длиннее ключи, нулевая длина, формат независим от разрядности | §3 |
| C. Транзакции и конкурентность | Парковка/вытеснение, клонирование, fork, HSR | §4 |
| D. GC и размер БД | Big Foot, LIFO/FIFO, авто-компактификация, дефрагментация | §5 |
| E. Долговечность | SAFE_NOSYNC, steady-точки, троика мета, восстановление | §6 |
| F. Движок B+tree | Умные split, merge-тактика, мультизначения, оценки | §7 |
| G. C API | Расширенные операции, информация, опции, кэш | §8 |
| H. C++ API | Полный типобезопасный слой | §9 |
| I. Утилиты и диагностика | chk/defrag/copy/dump, статистика, UUID | §10 |
| J. Оптимизации | SIMD, branchless, prefault, спилл, сортировки | §11 |
| K. Переносимость | Платформы, блокировки, TLS, endianness | §12 |
| L. Сборка/дистрибуция | Amalgamation, CMake/Make, лицензия, Conan | §13 |
| M. Экосистема | Биндинги, веб-сайт, CI, спонсорство | §14 |

Хронологическая таблица по версиям — §15.

---

## 2. Надёжность: устранённые дефекты, унаследованные от LMDB

**Известные и публично зафиксированные:**

- Утечки страниц БД и ошибочная статистика таблиц/под-БД.
- Segfault'ы в нескольких условиях (повреждённые БД, крайние случаи курсоров, DBI=0 в читающих
  транзакциях и т.п.).
- Неоптимальная стратегия слияния страниц при удалении (исправлена тактикой «предпочесть уже
  изменённую страницу», см. §5.5/§7).
- Обновление существующей записи с изменением размера данных (в т.ч. в multimap) — у LMDB может
  молча терять данные с большими значениями.
- Повреждение БД в режиме `DUPFIXED` при длинных/нечётных мультизначениях (LEAF2-страницы):
  ошибка присутствовала в LMDB более 11 лет и была унаследована; в libmdbx исправлена (0.12.10).
- «Реинкарнация» удалённой под-БД, неявное удаление через операции над `@MAIN`.
- Гонки при открытии DBI-хендлов и при старте транзакции параллельно с созданием дескриптора.
- Циклирование обновления GC при коммите (расходимость) — устранено корректирующей обратной связью.
- Ложное срабатывание `MDBX_CORRUPTED` при невыровненном доступе к 64-битным полям
  (в т.ч. на ARM и после `#pragma pack`).
- Ошибки копирования БД на NFS/CIFS/SMB (конфликт `fcntl`/`flock`, `EAGAIN`/`EWOULDBLOCK`).
- Потеря содержимого таблицы при abort вложенной транзакции, где таблица была удалена (0.14.x).
- Некорректное закрытие DBI-дескриптора изменённой таблицы (пустое имя, утечки, битый корень).
- Повторное использование/«воскрешение» закрытых курсоров вложенных транзакций (утечки, UAF).

**Системные гарантии надёжности (в отличие от LMDB):**

- **Целостность при асинхронной неупорядоченной записи** (`SAFE_NOSYNC`): в отличие от
  `MDB_NOSYNC`, база не разрушается при сбое — выполняется откат к последнему steady-коммиту;
  для полного соответствия поведению LMDB остаётся `MDBX_UTTERLY_NOSYNC`.
- **Контроль по `boot_id`** для решений об откате слабых мета-страниц (в т.ч. внутри LXC).
- **Защита от incoherence unified page/buffer cache** (issue #269, Linux) — полный workaround (0.11.5–0.11.6).
- **Проверка согласованности файловых систем**: отказ/предупреждение при работе с неподходящими
  ФС (эксклюзивный режим на сетевых шарингах разрешён, read-only кооперативно — тоже).
- **Чистка stale-читателей** при открытии и перед ростом БД; **авто-очистка stale-писателей**.
- **`MDBX_EMULTIVAL`** при неоднозначном обновлении/удалении (вместо тихого неверного поведения).
- Предотвращение повреждения из-за двойного открытия одной БД в процессе (отслеживание +
  восстановление POSIX-блокировок; legacy-режим совместимости — опционально).
- Возврат `ENOLCK` на WSL1 (где работа невозможна в принципе) вместо молчаливого повреждения.

---

## 3. Модель данных и формат

- **Ключи в >2 раза длиннее**, чем в LMDB: до ~½ страницы (2022 байта при 4K, 32742 при 64K)
  против 511 байт; лимиты вынесены в API (`mdbx_limits_*`, `mdbx_env_get_maxkeysize()` и др.).
- **Нулевая длина ключей и значений** (у LMDB ключ обязан быть ненулевой длины).
- **Формат БД одинаков для 32- и 64-битных сборок** (зависит только от endianness).
- Мультизначения: плотные dupfix-страницы фиксированной длины, вложенные деревья, суб-страницы;
  лимиты и глубина вложенности доступны через API (`dupsort_depthmask`, `count_ex`).
- **Последовательности (sequences)** и **три персистентных 64-битных маркера (vector-clock/canary)**.
- **UUID базы** в `mi_dxbid` (идентификация, а не только проверка сигнатуры).
- Внутренние размеры узлов уточнены: меньше overflow-страниц в ряде случаев, выше лимиты ключей.
- Непечатные имена таблиц поддерживаются; пустые имена обрабатываются корректно.
- Номер последней модифицирующей транзакции для каждой таблицы (per-subDB last-update txnid).

---

## 4. Транзакции и конкурентность

- **Парковка читающих транзакций** (`mdbx_txn_park`/`unpark`) с флагами `PARKED`/`AUTOUNPARK`/`OUSTED`,
  авто-перезапуск вытесненных (`restart_if_ousted`).
- **Вытеснение (ousting)** припаркованных читателей писателями (CAS `PARKED→OUSTED`).
- **Handle-Slow-Readers (HSR)** колбэк (эволюция старого `mdbx_env_set_oomfunc`): разрешение
  переполнения, вызванного долгими читателями.
- **Клонирование читающих транзакций** (`mdbx_txn_clone`).
- **`mdbx_env_resurrect_after_fork()`** — безопасное переоткрытие окружения в дочернем процессе.
- **Вложенные транзакции** (у LMDB есть, но здесь радикально переработаны): быстрый путь
  «чистой» вложенной транзакции, lazy-затенение курсоров, корректная судьба созданных/удалённых
  таблиц, поддержка в C++ API.
- **Вложенные read-only транзакции** и «фейковые» read-only (для единообразия API).
- `mdbx_txn_refresh()`, `mdbx_txn_checkpoint()` (коммит без снятия блокировки),
  `mdbx_txn_commit_embark_read()`, `mdbx_txn_amend()` (запись со снапшота чтения),
  `mdbx_txn_rollback()` (abort+restart без потери блокировки).
- `mdbx_txn_break()` — явная пометка транзакции «сломанной».
- **NOSTICKYTHREADS** вместо LMDB'шного `MDBX_NOTLS`: транзакции можно передавать между потоками
  (с проверками `MDBX_THREAD_MISMATCH`/`MDBX_TXN_OVERLAPPING`/`MDBX_BAD_RSLOT`/`MDBX_BUSY`).
- Явная регистрация/дерегистрация потоков-читателей (`mdbx_thread_register/unregister`).
- **Управление основной блокировкой** lock/unlock/upgrade/downgrade для сложных сценариев.
- Дисциплина курсоров: все курсоры **можно переиспользовать и требуется закрывать явно**
  (устраняет UAF/double-free); `mdbx_cursor_create/bind/unbind`, `mdbx_txn_release_all_cursors[_ex]`.
- Позиционирование курсоров операциями `<`, `<=`, `==`, `>=`, `>` (для пар ключ-значение тоже);
  `MDBX_SET_LOWERBOUND`, `MDBX_SET_UPPERBOUND`, `mdbx_cursor_compare` (оператор `<=>`),
  `mdbx_cursor_scan[_from]`, `on_first_dup`/`on_last_dup`.
- Курсоры, связанные с одной таблицей в разных read-транзакциях, допустимы в multi-cursor API.
- `mdbx_preopen_snapinfo()` — информация о БД без её открытия.

---

## 5. Переиспользование страниц (GC), рост и размер БД

- **Big Foot** (0.12.1): дробление больших списков retired-страниц на цепочки записей —
  GC больше не требует длинных последовательностей свободных страниц для огромных транзакций.
- **Early GC Cleanup** (0.14.x): утилизированные записи GC удаляются как можно раньше,
  а не только при коммите; открывает путь к дефрагментации и нелинейной переработке GC.
- **LIFO-политика** переиспользования (`MDBX_LIFORECLAIM`) для write-back кэша; FIFO по умолчанию.
- **Автоматическая on-the-fly подстройка размера БД** (рост и сокращение) через геометрию
  (`lower/now/upper/growth_step/shrink_threshold`).
- **Непрерывная zero-overhead компактификация**: возврат хвоста в неразмеченное пространство
  (refund) + усечение при накоплении (implicit shrink, `stockpile_gap`, MADV_DONTNEED/REMOVE).
- **Явная дефрагментация** (`mdbx_env_defrag`, утилита `mdbx_defrag`) с контролем целей/времени.
- **Динамические лимиты**: `rp_augment_limit` (авто-подстройка от размера БД),
  `gc_time_limit`, лимиты грязных страниц (`txn_dp_limit` — авто от объёма ОЗУ), спилл-деноминаторы.
- Авто-слияние записей GC; оптимизированный `pnl_merge`; профилирование GC (`MDBX_ENABLE_PROFGC`).
- Диагностика GC: `mdbx_gc_info()` (состояние, гистограммы, итерация записей), `gcrtime` счётчик.
- **Refund/loose-страницы**: возврат страниц внутри транзакции без обращения к GC
  (опции `MDBX_ENABLE_REFUND`, `MDBX_opt_loose_limit`, `dp_reserve_limit`).
- RKL (наборы идентификаторов записей GC: интервалы + списки) и «резервирование со взвешенным
  запасом» — одно-проходное обновление GC от `O(1)` до `O(log N)`.
- Разреженный/локфри DBI-наборы (`MDBX_ENABLE_DBI_SPARSE`, `MDBX_ENABLE_DBI_LOCKFREE`) для большого
  числа таблиц.
- Счётчики пространства в `mdbx_txn_info`: dirty/leftover/retired/limit для читающих и пишущих
  транзакций (инструмент борьбы с «долгими читателями»).

---

## 6. Долговечность и синхронизация

- **Три мета-страницы + метод фиксации «Troika»** (0.12.1): минимум барьеров памяти, сравнений и
  условных переходов; два устойчивых снапшота + хвостовой слот; двухфазное обновление.
- Явные **режимы синхронизации**: `MDBX_SYNC_DURABLE`, `MDBX_NOMETASYNC`, `MDBX_SAFE_NOSYNC`
  (переименование из `MDBX_NOSYNC`), `MDBX_UTTERLY_NOSYNC`; `MDBX_MAPASYNC` объявлен устаревшим.
- **Steady vs weak мета**: понятие устойчивой точки; автоматический steady-point при нехватке места.
- **Автоматическая синхронизация по порогам/таймауту** с дешёвым polling
  (`mdbx_env_set_syncbytes`/`syncperiod`, `MDBX_opt_sync_bytes/period`, `presync_threshold`,
  async `mdbx_env_sync[_ex|_poll]` с предварительной проверкой и ретраем).
- **Динамический выбор способа записи** (`MDBX_opt_writethrough_threshold`): write-through vs
  write+`fdatasync` в зависимости от числа страниц и задержки канала.
- **Prefault-запись** (`MDBX_opt_prefault_write_enable`) против page-fault'ов в `WRITEMAP`.
- Восстановление без WAL: выбор последней цельной меты, режим `open_for_recovery` и переключение
  на заданную мета-страницу; **гарантия не-изменения БД** при recovery-проверках (0.12.7+).
- `MDBX_WANNA_RECOVERY`/`MDBX_MVCC_RETARDED` диагностические коды.
- Контроль некогерентности unified page cache (см. §2).
- На macOS/iOS `fcntl(F_FULLFSYNC)` по умолчанию (опция скорости `MDBX_APPLE_SPEED_INSTEADOF_DURABILITY`).

---

## 7. Движок B+tree и страницы

- **Split с «auto-appending»**: вставка упорядоченных последовательностей ключей даёт более
  плотное заполнение страниц; **split «по середине»** для баланса дерева (0.10.0).
- **Тактика слияния при удалении**: предпочитать уже изменённую (грязную) страницу — снижение WAF
  до 50% при массовых удалениях; управление через `prefer_waf_insteadof_balance` и `merge_threshold`
  (по умолчанию 33% с 0.14.x).
- **Прозрачный спилл** грязных страниц (0.10.0): страницы готовы к выгрузке без повторного
  изменения; LRU-политика с приоритетом для overflow-страниц; спилл с учётом размера
  large/overflow; `MDBX_TXN_NIPPED` для паузы спилла при обработке GC.
- Массовые операции: **удаление «гроздьями»** (`mdbx_cursor_bunch_delete`, `delete_range`) —
  вырезание целых страниц/ветвей; **пакетное чтение** (`mdbx_cursor_get_batch`); multi-value
  пакеты (`MDBX_GET/PUT_MULTIPLE`, `SEEK_AND_GET_MULTIPLE`, put/seek samelength, batch put).
- **Оценка объёма диапазонных запросов** (`mdbx_estimate_range/distance/move`, `MDBX_EPSILON`)
  по общим страницам стеков курсоров.
- **Проверка страниц «на лету»**: улучшенная hot/online валидация страниц; опция
  `MDBX_VALIDATION` для работы с повреждёнными/недоверенными БД; обнаружение повреждений по
  `parent-page-txnid`.
- Параметры плотности: `MDBX_opt_subpage_*` (лимит суб-страниц, резерв), `split_reserve`.
- Трекинг курсоров в пишущих транзакциях (repoint/shadow), `mdbx_is_dirty()` для избегания
  копирования с грязных страниц.
- Минимизация чтения листовых страниц при удалении таблиц/вложенных деревьев.

---

## 8. C API: расширения

- **Единый API опций** `mdbx_env_set_option/get_option` с полным набором `MDBX_opt_*`:
  `max_db`, `max_readers`, `sync_bytes`, `sync_period`, `rp_augment_limit`, `loose_limit`,
  `dp_reserve_limit`, `txn_dp_limit`, `txn_dp_initial`, `spill_max_denominator`,
  `spill_min_denominator`, `spill_parent4child_denominator`, `merge_threshold`,
  `prefer_waf_insteadof_balance`, `writethrough_threshold`, `prefault_write_enable`,
  `gc_time_limit`, `split_reserve`, `subpage_limit`, `subpage_room_threshold`,
  `subpage_reserve_prereq`, `subpage_reserve_limit`, `presync_threshold`.
- **Расширенные CRUD**: put с получением предыдущего значения; обновление/удаление конкретного
  мультизначения; `MDBX_UPSERT`, `MDBX_ALLDUPS`, `MDBX_APPEND/DUP`, `MDBX_NOOVERWRITE`;
  upsert всех дубликатов; reserve; empty-data в `MDBX_MULTIPLE`.
- **«get-cached»**: `mdbx_cache_get[_SingleThreaded]` — ленивый поиск по версионным меткам страниц
  со статусами HIT/CONFIRMED/REFRESHED/DIRTY/BEHIND/UNABLE/RACE; разделяемый lock-free.
- Геометрия и лимиты: `mdbx_env_set_geometry`, `mdbx_limits_*` (keysize/valsize/pairsize
  `...4page_max`, min-границы), `mdbx_default_pagesize`, `mdbx_get_sysraminfo`.
- Информация: `mdbx_env_info_ex` (геометрия, меты, meta[3], UUID, статистика операций со страницами),
  `mdbx_txn_info` (включая `scan_rlt`), `mdbx_dbi_flags_ex` (состояние DIRTY/STALE/FRESH/CREAT),
  `mdbx_enumerate_tables`, `mdbx_cursor_count_ex` (мультизначения + стат. вложенного дерева).
- Транзакции: весь набор из §4; `mdbx_txn_commit_ex` с метриками задержек; `mdbx_txn_copy2pathname/fd`.
- Таблицы: `mdbx_dbi_rename[_2]`, `mdbx_dbi_sequence`, канарейки `mdbx_canary_*`,
  `mdbx_drop` API, deferred-инвалидация хендлов удалённых таблиц (0.14.3+), `MDBX_DB_ACCEDE`.
- Окружение: `mdbx_env_delete` (мультипроцессное удаление), `mdbx_env_warmup`,
  `mdbx_env_chk` (проверка изнутри библиотеки), `mdbx_env_defrag`, `mdbx_env_sync_poll`,
  exclusive/read-only/без-LCK режимы, `mdbx_env_get_path[_W]`, `mdbx_set_panic`,
  user-context для транзакций и курсоров.
- Преобразования ключей: value-to-key для чисел (int32/int64, float/double, JSON-integer),
  `mdbx_key_from_*`; рекомендация вместо кастомных компараторов.
- Логирование: callback без `vprintf` (удобно для биндингов), `mdbx_assert_fail` в public API,
  настройка уровня через env-переменные, уровни `MDBX_LOG_DEBUG/TRACE` для ошибок API.
- Диагностика времени выполнения: `mdbx_get_sysraminfo`, page-op статистика через
  `mdbx_env_info_ex`, `MDBX_ENABLE_PGET_STAT` счётчик обращений к страницам.

---

## 9. C++ API

- Полный типобезопасный RAII-слой: `env`/`env_managed`, `txn`/`txn_managed`,
  `cursor`/`cursor_managed`, `map_handle`, `slice`/`buffer<>` (полиморфные аллокаторы C++17,
  политики владения, `inplace_storage_size_rounding`).
- Иерархия исключений, сопоставленная кодам C API; `make_broken()`; `[[nodiscard]]`-подобные
  конвенции; C++20 concepts при наличии.
- Типизированные map-операции, `mdbx::pair`, key/value трансляции (включая value2key).
- Кодировщики: hex/base58/base64 (высокопроизводительный base58, RFC-черновик),
  `is_printable`, UTF-8 проверки, безопасные middle/резервирование.
- `mdbx::comparator`, `default_comparator`, `estimate_result`, `extra_runtime_option`,
  геометрия с fluent-сеттерами, commit-метрики задержек.
- Вложенные пишущие транзакции, buffer append/reserve, `get_/set_context`,
  move/copy assignment для управляемых классов, `withdraw_handle`.
- Явная инстанциация шаблонов внутри библиотеки (быстрее сборка потребителей).

---

## 10. Утилиты и диагностика

- `mdbx_chk`: глубокий контроль (мета/троика, деревья, порядок ключей, GC, scopes,
  гистограммы заполнения страниц и мультизначений), проверка по указанной мета-странице,
  переключение мета-страниц, опции `-u/-U` (warmup), verbosity, «выживание» при повреждённых
  деревьях.
- `mdbx_copy`: hot backup (в т.ч. в pipe), copy-with-compaction (зануление неиспользуемых
  промежутков), `-d/-p/-f` опции, overwrite.
- `mdbx_defrag` (новое): цели/лимиты дефрагментации.
- `mdbx_dump`/`mdbx_load`: полная поддержка атрибутов, компактный режим `-c` (однократные ключи),
  purge `-p`, batch-insert `-b`, лимиты `-L`, плотность `-d`, геометрия `-G`.
- `mdbx_drop`, `mdbx_stat` (включая `-p` page-op статистику и все счётчики), `mdbx_test`
  (стохастические сценарии, `--geometry-jitter`, `--numa`, `--pagesize`, `--loglevel`),
  `stochastic.sh`, `battery-tmux.sh`.
- Версия/сборка: `VERSION.json`, `SOURCE_DATE_EPOCH`, `MDBX_BUILD_TIMESTAMP`, `MDBX_BUILD_METADATA`,
  вывод `options:` для совместимости хоста/контейнеров.

---

## 11. Оптимизации производительности

### 11.1. Микрооптимизации движка

- **SIMD-поиск последовательностей свободных страниц**: SSE2/NEON/AVX2/AVX512 ядра
  (`scan4seq_*`) — ускорение ×4/×8/×16 (0.12.1); починка NEON для ARM64-Windows.
- **Branchless бинарный поиск** (bsearch/lower_bound на CMOV) с workaround'ом бага CLANG x86;
  позже — **встраивание кода встроенных/дефолтных компараторов** (0.14.2).
- **Сортировки**: адаптивная бинпоиск-сортировка, radix-sort (LSB-first, 2×16-бит цифры),
  сортировочные сети для n=3..8, branch-free compare-swap; быстрые сортировки списков страниц
  (PNL/DPL); порог переключения radix (`MDBX_RADIXSORT_THRESHOLD`).
- **Лениво-сортируемый список грязных страниц (DPL)** с сортировкой по требованию;
  оптимизированный `dpl_append`.
- **C11-атомики** для слабых моделей памяти (ARM/AArch64/PPC/MIPS/RISC-V): acquire/release,
  CAS, safe64-протокол чтения 64-бит, проверка атомарности на сборке; нетяжёлые fenced.
- Рациональное использование барьеров/атрибутов: `pure`/`const`, `__cold`/`__hot`,
  `__always_inline`, `likely/unlikely` (разметка горячих/холодных путей).
- `__builtin_cpu_supports` для диспетчеризации SIMD (`MDBX_HAVE_BUILTIN_CPU_SUPPORTS`).
- Минимизация системных вызовов: отказ от `pwritev` для одиночных записей, объединение
  регионов записи, избегание лишних `msync`, отказ от `copy_file_range` на дефектных ядрах,
  `fallocate` против SIGBUS, `fcntl64` для блокировок.
- **Prefault-запись** и **mincore**-отслеживание резидентности страниц для предотвращения
  page-fault'ов в `WRITEMAP`.
- Отсутствие операций с плавающей точкой и зависимости от `libm` (0.14.x); 16.16 fixed-point
  для времени/порогов.
- Атрибуты TLS (`tls_model("local-dynamic")`), аккуратная работа с деструкторами TLS.
- `-fno-semantic-interposition` для снижения накладных расходов на вызовы собственных функций.

### 11.2. Алгоритмические улучшения

- Прозрачный спилл + LRU (см. §7), refund/loose, auto-merge записей GC, одно-проходный GC-update
  через RKL (см. §5).
- Ускорение GC-update для огромных транзакций (Ethereum/Erigon) — существенно (0.11.3).
- Auto-appending split и «split по середине»; merge с грязным соседом (см. §7).
- Динамические эвристики: авто-подстройка `dp_limit` от ОЗУ, `rp_augment_limit` от размера БД,
  автоподбор страницы и геометрии по умолчанию.

### 11.3. Синхронизация/параллелизм

- Wait-free читатели без атомик на пути чтения; lock-free сканирование таблицы читателей;
  кэширование старейшего читателя.
- OFD-блокировки (с fallback на POSIX/`fcntl64`), таймаутные ожидания на Windows,
  SysV/semafor варианты, режим без LCK-файла.
- Overlapped/async запись на Windows (`WriteGather`, unbuffered I/O) — кратный выигрыш в
  ряде сценариев против LMDB.

---

## 12. Переносимость и платформы

- Платформы: Linux, Windows, macOS/iOS, Android, Harmony OS, Haiku, FreeBSD, NetBSD, OpenBSD,
  DragonFly, Solaris/OpenIndiana/OpenSolaris, Plan 9/9P (эксклюзивный режим), WSL2 (и корректный
  отказ на WSL1); поддержка GNU Make + CMake + MinGW + MSVC + CLANG + GCC + Elbrus/LCC.
- Weak memory model поддержка (см. §11.1); big-endian и нестандартные размеры страницы;
  большие БД (>4 ГБ) из 32-битного кода.
- Обработка специфики ОС: Wine workarounds, DrvFs, NFS/CIFS/SMB, CDROM, `F_FULLFSYNC`,
  `GetExitCodeThread`, boot_id на Windows/LXC.
- Защита от повторного использования pid/tid; liveness-проверки читателей; безопасный `fork`.

---

## 13. Сборка, дистрибуция, лицензия

- **Amalgamated single-file дистрибутив** (как SQLite) с отрезаемыми dev-маркерами;
  `make dist`, пакеты, Conan-рецепт.
- CMake (в т.ч. подпроект), GNU Make, широкий набор build-опций (включая
  `MDBX_WITHOUT_MSVC_CRT`, `MDBX_CHECKING`, `MDBX_VALIDATION`, `MDBX_ENABLE_*`, `MDBX_AVOID_MSYNC`,
  `MDBX_BUILD_TOOLS`, `MDBX_USE_OFDLOCKS`, `MDBX_FORCE_ASSERTIONS`(deprecated)).
- Воспроизводимые сборки (`SOURCE_DATE_EPOCH`/`MDBX_BUILD_TIMESTAMP`), LTO, ASAN/UBSAN/MSAN
  (Valgrind/ASAN), `ctest`, проверки атомарности.
- Лицензия Apache-2.0 (с 0.13), объяснение в COPYRIGHT.
- Отдельные config-файлы для GNU Make и CMake; ограничение утечек внутренних символов.

---

## 14. Экосистема

- Официальный веб-сайт с Doxygen-документацией, FAQ, Tips, Knowledge Base, биндингами.
- Биндинги: Rust, Go, Java, C#, .NET, Python, Ruby, Haskell, Lua, Deno, NodeJS, Nim, Zig и др.
- Использование в Ethereum-экосистеме (Erigon, Reth, Silkworm), Fast Positive Tables.
- CI: GitHub Actions, SourceCraft, CMake/CTest, кросс-сборки, статические анализаторы (CodeQL,
  Coverity), fuzzing.
- Спонсорство/фондирование (Positive Technologies, Erigon).

---

## 15. Хронология по версиям (ключевые вехи)

| Версия (год) | Ключевые доработки |
| --- | --- |
| 2015–2017 (ReOpenLDAP/ранний) | Форк; базовая надёжность, более длинные ключи, нулевые ключи |
| ~0.9.x (2019–2021) | API опций; динамические списки; переработанный спилл; refund; C11-атомики; C++ API (0.9.1); `env_delete`, `commit_ex`, `SET_LOWERBOUND` |
| 0.10.x (2021) | `set/get_option`; прозрачный спилл + LRU; auto-appending split; `get_sysraminfo`; `DISABLE_PAGECHECKS`; статистика page-op |
| 0.11.x (2021–2022) | `cursor_get_batch`, `SET_UPPERBOUND`; ускорение GC для огромных транзакций; fix incoherent page cache (#269); переезд после удаления GitHub; wchar API; C++ finalized |
| 0.12.x (2022–2023) | **Big Foot**; **Troika**; SIMD-поиск (AVX2/AVX512/SSE2/NEON); branchless bsearch; prefault-write; writethrough; merge-тактика; `warmup`; Windows overlapped-I/O; LCK v2; `VALIDATION` |
| 0.13.x (2023–2025) | Apache-2.0; **парковка/вытеснение**; HSR; DBI sparse/lockfree; `gc_time_limit`; `env_chk` в библиотеке; rename; cursor scan/compare; `resurrect_after_fork`; `NOSTICKYTHREADS`; subpage-опции; `prefer_waf_insteadof_balance`; UUID; большая серия API-расширений |
| 0.14.x (2025–2026) | **Early GC cleanup**; **явная дефрагментация** + `mdbx_defrag`; `bunch_delete`/`delete_range`; `cache_get`; `txn_clone/refresh/checkpoint/amend/rollback/embark_read`; вложенные read-only; `gc_info`; `split_reserve`; distance/scroll/distribute; Harmony OS/Haiku; без float/`libm`; `MDBX_CHECKING`; branchless + встроенные компараторы; `presync_threshold`; deferred-инвалидация DBI; стабилизация 0.14.x (0.14.3) |

---

## 16. Статус и дальнейшие шаги

Это первая (каталожная) версия. Дальнейшие шаги:

1. Сверить пункты с кодом для категорий, где это критично (формат, опции, мета).
2. Добавить ссылки на разделы [`functional-architecture.md`](functional-architecture.md), где
   механизмы описаны подробно.
3. При необходимости — отдельный документ «что именно отличается в поведении по умолчанию».