# Probe-инструментарий libmdbx: probe-bus (mprobe v2) и USDT/DTrace-маркеры

> Объединённый документ. Описывает **движковый** механизм управляемого
> тестирования `MDBX_PROBES` — probe-bus (mprobe v2) — и каталог внешних
> статических трассировочных пробников `USDT`/`SystemTap`/`DTrace`,
> встроенных в движок. Разделение ролей:
> per-site USDT-маркеры нужны внешней трассировке в эксплуатации и
> при тестировании через SystemTap; единый `mprobe_event` от probe-bus —
> для внутренних инъекций/наблюдения в тестах (root и внешний
> инструментарий не требуются).
>
> Методология осей (COLLECT/WATCH/FAULT) — в
> `docs/engineering/testing-methodology.md` (гл. 9–12 и §CH-9/CH-11);
> справка по языку SystemTap — в
> `docs/engineering/systemtap-reference.md`.
> Probe-bus — преемник первой версии `tests/tracing/mprobe.h` (v1, удалена
> 2026-09-30): та же таксономия, но без внешних трассировщиков — реестр
> в процессе, детерминированная инъекция ошибок, управление из любого теста.

## 1. Зачем

- **Достижимость «100% покрытия»**: девиантные ветви (ENOMEM, MDBX_TXN_FULL,
  PROBLEM, defensive error-path'ы) недостижимы штатными вызовами — их надо
  провоцировать детерминированно.
- **Девиантные проверки**: `DEV_ASSERT` отличает «ситуация валидна, но девиантна
  у вызывающего» от инварианта движка; в probe-сборках такие проверки можно
  выборочно отключать/подсчитывать, не трогая глобальные ASSERT.
- **Самоутверждение покрытия**: счётчики `seen/hits/suppressed` позволяют тесту
  проверять, что ветка/точка действительно была пересечена — без внешнего gcov.
- **Наблюдение в проде**: статические USDT-пробники дают эксплуатации три
  возможности (наблюдать путь исполнения, читать значения на границах фаз,
  инъектировать ошибки через SystemTap) — см. §11.

## 2. Активация (probe-bus)

| Уровень | Как |
| --- | --- |
| Сборка | CMake: `-DMDBX_PROBES=ON` (dev-only; в dist OFF, код вырезается dist-cutoff'ом) |
| Runtime | env `MDBX_PROBES=1` **или** `MDBX_PROBE_CTL=<dir>` (включает и файловое IPC) |

Без runtime-активации весь механизм — один `unlikely()`-проверочный ветвь на
пробник/аллокацию (нулевая стоимость). В производстве опция не включается;
dist-вырезка проверена (`grep mprobe/MDBX_PROBES dist/*` = пусто).

## 3. Макросы (таксономия)

| Макрос | Канал | Семантика |
| --- | --- | --- |
| `MPROBE_COLLECT(name, value)` | метрики | seen/hits++, сохраняет `value` (последнее) |
| `MPROBE_WATCH(name, value)` | событие | seen/hits++, фиксирует факт прохождения ветви |
| `MPROBE_FAULT(name, var)` | инъекция | если взведено правило `fault <tag> <code>` — `var` мутируется кодом ошибки; иначе только наблюдение |
| `DEV_ASSERT(expr)` / `DEV_ASSERT_T(tag, expr)` | девиантная проверка | при false: seen++; если armed — hits++ и действие по `mode`, иначе suppressed++ |

`name`/`tag` — **семантические теги** (стабильны при рефакторинге), не номера
строк. Конвенция имён — как у USDT-маркеров, см. §11.2
(`mdbx::<subsystem>::<phase>::<event>` или краткий `symbol_tag`).

Вне probe-сборок (`MDBX_PROBES` не определён) `DEV_ASSERT*` сводятся к `CHECK0()`
(= исторический ASSERT), `MPROBE_*` — к no-op: поведение dist не меняется.

> **Расстановка зондов — без препроцессора.** Новые зонды добавляются только
> прямым вызовом `MPROBE_*`/`DEV_ASSERT*` в точке, **без** `#if defined(MDBX_PROBES)`
> и любых других `#if/#ifdef/#ifndef/#elif` вокруг них: в не-probe сборках
> макросы — no-op, поэтому гейтинг на уровне препроцессора не нужен и вреден.
> Выбор «probes-only» поведения выполняется в рантайме (arm/disarm), а сборка
> тестов под probe-bus — на уровне CMake (`if(MDBX_PROBES)`), не в коде.
> См. также общее правило минимизации `#if` в `AGENTS.md`.

## 4. Реестр

Процессовый реестр записей, ключ — семантический тег (dedup: все якоря с одним
тегом разделяют одну запись — включая white-box копии движка, скомпилированные
и в libmdbx, и в тестовый бинарь). Поля записи (словоразмерные атомики):

```
flags       bit0 ARMED, bit1 FAULT_ONCE
seen        сколько раз точка оценена
hits        сколько раз сработала (для assert — число падений)
suppressed  сколько раз «сработала бы», но disarmed
value       последнее захваченное значение
fault_code  инъектируемый код (0 = нет)
```

Регистрация ленивая, без аллокаций (структуры статические per-call-site),
ограничена `MPROBE_MAX_SITES` (512). Счётчики — `mdbx_atomic_size_t`
(32-битные на 32-битных платформах).

## 5. Управление: `mprobe_ctl()`

`int mprobe_ctl(const char *request, char *reply, size_t reply_size);`

Строковый протокол (ops), reply: `ok\n` + payload или `err <msg>\n`:

| op | аргументы | действие |
| --- | --- | --- |
| `list` | — | перечислить зарегистрированные теги |
| `arm` / `disarm` | `<pattern>` | взвести/снять ARMED на совпавших тегах (`*` = суффикс-глоб) |
| `mode` | `panic`\|`log`\|`count` | глобальный режим действия для assert |
| `fault` | `<pattern> <code>\|<none>` | задать/снять инъекцию кода возврата на FAULT-точках |
| `alloc-fault` | `<count>\|<none>` | провалить следующие N аллокаций (redirect `osal_*`) |
| `query` | `<pattern>` | dump: `site <name> <kind> <armed> <seen> <hits> <suppressed> <value> <file>:<line>` |
| `reset` | `<pattern>` | обнулить счётчики |
| `sync` | — | барьер для файлового IPC (см. ниже) |

### 4.1 Интеграция с MCP (shared-graph-memory)

Каждый зонд регистрируется в персистентной памяти роя (таблица `probes`,
MCP-инструменты `probe_add`/`probe_batch`) — тег, kind, mechanism, file:line,
status. Это даёт:

- **единый реестр** зондов, видимый всем агентам (SystemMap): «где какая
  точка, что можно инъектировать»;
- **поведенческую паритетность C→C++**: сравнение трасс зондов до/после
  переписывания;
- **авто-сбор**: `tools/scan_probes.py` извлекает `MPROBE_*`/`DEV_ASSERT*` из
  `src/**` и USDT-каталог из §11.4 → `tools/artifacts/probes.json` →
  `Store.probe_batch` (или `--db`).

**Правило для агента**: при добавлении нового зонда/`DEV_ASSERT` в код —
зарегистрируйте его в MCP через `probe_add(tag, kind, mechanism, symbol,
file, line)` сразу после коммита (либо обновите реестр прогоном
`scan_probes.py`). Тег — тот же семантический тег из кода. Это сохраняет
реестр актуальным и позволяет тестам ссылаться на зонды по тегу.

## 6. Транспорты

### 6.1 In-process (основной)

Тест вызывает `mprobe_ctl()` напрямую — он линкует libmdbx. Детерминированно:
запрос применяется синхронно до возврата.

### 6.2 Файловое IPC

env `MDBX_PROBE_CTL=<dir>`: запросы в `<dir>/cmd` (по строке), ответы в
`<dir>/rep` (append). Библиотека сливает `cmd` лениво — на probe-срабатываниях
и на каждом `mprobe_ctl()` (с отслеживанием смещения, повторное применение
исключено). **Детерминированный контракт**: записать строки в `cmd` → вызвать
`mprobe_ctl("sync")` (слив + ack) → выполнить действие.

Используется stdio (`fopen/fgets/fwrite`); при `MDBX_WITHOUT_MSVC_CRT`
(нет CRT stdio) канал деградирует в no-op, in-process `mprobe_ctl` работает.

## 7. Инъекция ошибок

- **Код возврата** (`MPROBE_FAULT` + `fault <tag> <code>`): на точке с
  взведённым правилом `*(int *)var = code` (например `MDBX_TXN_FULL`,
  `MDBX_ENOMEM`). При `mode count`/disarm — только подсчёт.
- **Аллокации** (`alloc-fault <N>`): под `MDBX_PROBES` `osal_malloc/realloc/
  calloc` перенаправляются на `mprobe_alloc/realloc/calloc`, которые при
  остатке >0 возвращают NULL и декрементируют счётчик. Настоящие аллокаторы
  сохранены как `osal_*_raw` (позволяет избежать рекурсии).
  Ограничение: `MDBX_WITHOUT_MSVC_CRT` не перехватывается.

## 8. DTRACE/внешние трассировщики

При срабатывании эмитится один общий маркер
`MDBX_DTRACE3(mprobe_event, name, file, line)` — внешний SystemTap/DTrace
может наблюдать события без per-site USDT. Управление/инъекция остаются за
реестром (внутри процесса) — внешний root/инструментарий не нужен.
Per-site USDT-маркеры для внешней трассировки описаны в §11.

## 9. Самопроверки

| Тест | Что проверяет |
| --- | --- |
| `extra_probes` (`tests/ut/api/probes.c`) | реестр/list/query/reset/arm/disarm, fault-инъекция кода, alloc-fault, mode count, DEV_ASSERT-подсчёт |
| `extra_probes_ipc` (`tests/ut/api/probes_ipc.c`) | файловое IPC round-trip (cmd→sync→rep) |

CI: `.github/workflows/ci-probes.yml` (ubuntu/windows/macos).

## 10. Где в коде

- `src/logging_and_debug.h` — макросы, `struct mprobe_site`, объявления API
  (внутри `dist-cutoff-begin/end` + `#if defined(MDBX_PROBES)`); fallback
  `DEV_ASSERT*`/`MPROBE_*` вне cutoff (нужны для сборки амальгамы).
- `src/logging_and_debug.c` — реализация (реестр, ctl, file-IPC, DTRACE,
  alloc-обёртки), внутри cutoff.
- `src/osal.h` — redirect аллокаций под `MDBX_PROBES` + `osal_*_raw`, в cutoff.
- `src/atomics-types.h` / `src/atomics-ops.h` — `mdbx_atomic_size_t` и helpers.
- `CMakeLists.txt` — option `MDBX_PROBES` (OFF, dist-cutoff);
  `tests/CMakeLists.txt` — регистрация самопроверок.
- GNUmakefile: dist-cutoff вырезается и из `mdbx-internals.h`, и из `mdbx.c`
  (рецепты амальгамации).

## 11. USDT/DTrace-маркеры (внешние трассировщики)

> Каталог и описание статических трассировочных пробников (`USDT`/
> `SystemTap`/`DTrace`), встроенных в движок. Наполняется в рамках
> **TASK-14** (`B3` бэклога). Терминология и сценарии использования — в
> `docs/engineering/test-scenarios.md` (§2, §3).

### 11.1 Зачем

Статические пробники дают тестам и эксплуатации три возможности (см.
test-scenarios §2.2):

1. **Наблюдать путь исполнения** — на какие ветви попало выполнение
   (страница из GC vs хвост файла vs рост файла; включился ли спилл; ...).
2. **Читать значения** локальных переменных/счётчиков на границах фаз.
3. **Инъектировать ошибки** — SystemTap `return`-пробник может переписать
   возвращаемое значение (эмуляция сбоя), см. §11.5.

### 11.2 Соглашение об именах

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

### 11.3 Как включить

- CMake: `cmake -DENABLE_SYSTEMTAP=ON` (требует `sys/sdt.h`; проверка в
  `cmake/profile.cmake`). Опция `ENABLE_DTRACE=ON` эквивалентна на платформах
  с `<sys/sdt.h>`.
- **Статус валидации платформ:** провалидирован только **Linux** — сборка
  с маркерами и их проверка (`readelf -n`, секция `.note.stapsdt`) выполнены
  на Linux/ELF. macOS/*BSD/Solaris-варианты имеют собственный инструментарий
  DTrace и **не** производят ELF-секцию `.note.stapsdt`; они не проверялись
  и пока считаются «по замыслу» (см. §11.6).
- GNUmakefile: цель `cmake-probes-build` собирает с `-DENABLE_SYSTEMTAP=ON` и
  выводит число маркеров.
- Без опций пробники компилируются в `__noop` (ноль-стоимость, без `.note.stapsdt`).

### 11.4 Каталог пробников

| Маркер | Модуль/функция | Аргументы | Смысл |
| --- | --- | --- | --- |
| `mdbx:alloc__source` | `gc-get.c` → `gc_alloc_ex()` (после выбора страницы) | `pgno` (u64), `mode` (u32) | Источник страницы: `mode=0` — из GC (reclaimed/loose), `mode=1` — хвост неразмеченного пространства (tail), `mode=2` — файл вырос (`dxb_resize`). `pgno` — номер первой страницы. Позволяет тесту отличить «переиспользование» от «роста» (сценарий SC-1). |
| `mdbx:commit__gc_update__begin` | `gc-put.c` → `gc_update()` (вход) | `txnid` (u64), `loop` (u32) | Начало цикла обновления GC-дерева при коммите; `loop` — номер итерации (0 первая). |
| `mdbx:commit__gc_update__end` | `gc-put.c` → `gc_update()` (выход) | `txnid` (u64), `rc` (i32) | Конец обновления GC; `rc` — код возврата (0 = успех). Готовая точка инъекции: перезапись `rc`. |
| `mdbx:spill__trigger` | `spill.c` → `spill_slowpath()` (перед ветвлением WRITEMAP/non) | `need_spill` (u64), `dirty_entries` (u64), `dirty_npages` (u64) | Сработал порог спилла; сколько нужно освободить и сколько грязных записей/страниц есть. |
| `mdbx:txn__write_started` | `txn-basal.c` → `basal_start_locked()` (после захвата) | `txnid` (u64), `front_txnid` (u64) | Пишущая транзакция стартовала (захвачен wrt_lock, выбран снапшот и расчётный id). `front_txnid` — «призрачный» id незакоммиченных страниц. |
| `mdbx:panic` | `logging_and_debug.c` → `panic_internal()` | `func` (ptr), `line` (u32), `msg` (ptr), `obj_class` (ptr), `obj` (ptr) | Аварийная остановка движка. |

### 11.5 Инъекция ошибок через SystemTap

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
пробников используется `tests/probes-check.sh` (см. §11.6).

### 11.6 Проверка без рантайма SystemTap

- `readelf -n <libmdbx.so>` показывает секцию `.note.stapsdt` со всеми маркерами.
- `tests/probes-check.sh <elffile>` — проверяет наличие ожидаемого набора
  маркеров, пригоден для CI (не требует root и `stap`).

> Справка по языку SystemTap (user-space/kernel домены, statement-пробники,
> наблюдение vs инъекция, требования/права): `docs/engineering/systemtap-reference.md`.

### 11.7 Кандидаты следующих партий (test-scenarios §2.4)

- `mdbx::reader::snapshot_begin/end` (txnid, число ретраев) — снятие снапшота читателя.
- `mdbx::commit::refund` (кол-во возвращённых страниц) — сработал refund.
- `mdbx::meta::two_phase::validate` (txnid_a/b, steady/weak) — двухфазное обновление меты.
- `mdbx::gc::detent_update` (oldest_txnid) — детент двинулся (SC-1).
- `mdbx::reader::slot_release` (tid) — освобождение слота читателя (SC-1).
- `mdbx::recovery::meta_selected` (мета, stuck_meta) — открытие/восстановление.
- `mdbx::bigfoot::chain` (длина цепочки, эффективный txnid) — bigfoot-цепочки (SC-2).
- `mdbx::hsr::kick` (pid, lag, space_retired) — отложенные читатели.