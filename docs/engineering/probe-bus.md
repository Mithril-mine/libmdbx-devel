# Probe-bus (mprobe v2): управляемая инъекция и наблюдение в тестах

> **Движковый** механизм управляемого/контролируемого тестирования
> (`MDBX_PROBES`, dev-only). Преемник первой версии `tests/tracing/mprobe.h`
> (v1, удалена 2026-09-30): та же таксономия `COLLECT/WATCH/FAULT`, но без
> внешних трассировщиков — реестр в процессе, детерминированная инъекция
> ошибок, управление из любого теста. Методология осей — в
> `docs/engineering/testing-methodology.md` (гл. 9–12); каталог внешних
> USDT/DTrace-маркеров — в `docs/engineering/probes.md`.

## 1. Зачем

- **Достижимость «100% покрытия»**: девиантные ветви (ENOMEM, MDBX_TXN_FULL,
  PROBLEM, defensive error-path'ы) недостижимы штатными вызовами — их надо
  провоцировать детерминированно.
- **Девиантные проверки**: `DEV_ASSERT` отличает «ситуация валидна, но девиантна
  у вызывающего» от инварианта движка; в probe-сборках такие проверки можно
  выборочно отключать/подсчитывать, не трогая глобальные ASSERT.
- **Самоутверждение покрытия**: счётчики `seen/hits/suppressed` позволяют тесту
  проверять, что ветка/точка действительно была пересечена — без внешнего gcov.

## 2. Активация

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
строк. Конвенция имён — как в `docs/engineering/probes.md`
(`mdbx::<subsystem>::<phase>::<event>` или краткий `symbol_tag`).

Вне probe-сборок (`MDBX_PROBES` не определён) `DEV_ASSERT*` сводятся к `CHECK0()`
(= исторический ASSERT), `MPROBE_*` — к no-op: поведение dist не меняется.

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

> Разделение ролей: per-site USDT-маркеры для внешней трассировки
> (SystemTap/DTrace) описаны в [`probes.md`](probes.md); единый
> `mprobe_event` от probe-bus — для внутренних инъекций/наблюдения в тестах.

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