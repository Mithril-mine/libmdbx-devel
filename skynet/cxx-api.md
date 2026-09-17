# C++ API Index (mdbx.h++ / mdbx++)

> Part of the [Skynet project index](README.md).
> Detailed map of the C++ API layer for refactoring: header organization, class inventory,
> decl/impl split, the C↔C++ bridge patterns, build & amalgamation integration, test coverage
> and compiler-support matrix. Companion: [`structure.md`](structure.md) §3 (file roles).

---

## 1. Topology

```
mdbx.h++            — single public C++ header (amalgamated-style even in the dev tree)
├── mdbx++/begin.h++            преамбула: макросы, типы, forward-объявления, enum endian
├── mdbx++/decl_exceptions.h++  error / exception / fatal / exception_thunk / MDBX_DECLARE_EXCEPTION
├── \defgroup cxx_data "Slices and Buffers"
│   ├── mdbx++/decl_slice.h++       slice (struct : MDBX_val), value_result, pair, pair_result
│   ├── mdbx++/decl_transcoders.h++ to_hex/from_hex, to_base58/from_base58, to_base64/from_base64 (SliceTranscoder)
│   └── mdbx++/decl_buffer.h++      buffer<ALLOCATOR,CAPACITY_POLICY> : slice (+ allocator traits)
├── \defgroup cxx_core "Environment, Transactions, Cursors, Key-value tables and map handles"
│   ├── mdbx++/decl_core.h++        cache_entry, loop_control, key_mode, value_mode, put_mode, map_handle
│   ├── mdbx++/decl_env.h++         env, env_managed (+ geometry/mode/durability/operate_*/limits/reader_info)
│   ├── mdbx++/decl_txn.h++         txn, txn_managed
│   └── mdbx++/decl_cursor.h++      cursor, cursor_managed (+ move_operation/result types)
├── inline bodies (порядок обязателен: decl раньше impl)
│   ├── mdbx++/impl_core.h++        impl_exceptions.h++  impl_slice.h++  impl_buffer.h++
│   └── mdbx++/impl_env.h++         impl_txn.h++         impl_cursor.h++
└── mdbx++/end.h++             эпилог
```

**Инвариант порядка**: `decl_*.h++` определяют типы до того, как `impl_*.h++` определяют их
методы; `begin.h++` держит все forward-объявления (`slice`, `buffer`, `env`, `env_managed`,
`txn`, `txn_managed`, `cursor`, `cursor_managed`). Нарушение порядка включений ломает сборку.

**Стратегия кода**: почти всё тело инлайн в заголовках (`impl_*.h++`); неинлайн — только
«холодные» части в `src/mdbx.c++` (1935 строк, §3). Следствие для рефакторинга: правка
`impl_*` перекомпилирует всех потребителей (большой радиус).

## 2. Class inventory (ключевые сущности)

| Тип | Файл | Роль / ключевые члены |
| --- | --- | --- |
| `slice` (struct : `::MDBX_val`) | `decl_slice.h++` | view на ключ/значение; `is_printable()` (неинлайн), транскодирование через `operator<<`/`operator>>`; результат-типы `value_result {value, done}`, `pair {key,value}`, `pair_result` |
| `buffer<ALLOCATOR, CAPACITY_POLICY>` : `slice` | `decl_buffer.h++` | владеющее хранилище; `enum modality {reference, inplace, allocated}`; EBCO-структура `silo : allocator_type`; аллокаторные адаптеры `move_assign_alloc`, `copy_assign_alloc`, `swap_alloc`, `default_capacity_policy`; `data_preserver` для отложенного копирования |
| `to_hex/from_hex`, `to_base58/from_base58`, `to_base64/from_base64` | `decl_transcoders.h++` | кодек-структуры, удовлетворяющие концепту `SliceTranscoder` |
| `env` / `env_managed : env` | `decl_env.h++` | окружение (RAII для managed); вложенные: `geometry` (+`size` для ostream), `enum mode {readonly,...}`, `enum durability {robust_synchronous,...}`, `reclaiming_options`, `operate_options`, `operate_parameters`, `limits`, `remove_mode`, `enum class extra_runtime_option` (обёртка `MDBX_option_t`), `reader_info`; `env_managed::create_parameters` |
| `txn` / `txn_managed : txn` | `decl_txn.h++` | транзакции; `start_nested([readonly])`, `commit_embark_read`, `drop_map/clear_map/rename_map` (неинлайн), CRUD-обёртки |
| `cursor` / `cursor_managed : cursor` | `decl_cursor.h++` | курсоры; `enum move_operation {first=MDBX_FIRST, ...}`, `move_result : pair_result`, `estimate_result : pair {approximate_quantity}`; шаблоны `scan_until(predicate, start, turn)` (мост через `exception_thunk`) |
| `error`, `exception : std::runtime_error`, `fatal : exception` | `decl_exceptions.h++` | ошибки/исключения; `MDBX_DECLARE_EXCEPTION(NAME)` генерирует производные (например `map_full`, `bad_dbi`); `exception_thunk` — ловушка исключений из C-колбэков |
| `cache_entry : MDBX_cache_entry_t` | `decl_core.h++` | запись кэша get-cached (initial draft) |
| `map_handle` (+`flags`, `info`, `state`) | `decl_core.h++` | дескриптор таблицы; `using state = ::MDBX_dbi_state_t` |
| `enum key_mode`, `enum value_mode`, `enum put_mode`, `enum loop_control` | `decl_core.h++` | режимы ключей/значений/вставки, управление циклами (`continue_loop=0`, `exit_loop=INT32_MIN`) |
| `enum endian` | `begin.h++` | little/big/native (из `__BYTE_ORDER__`) |

## 3. Неинлайн-реализации: `src/mdbx.c++` (1935 строк, 39 точек)

Паттерн: методы `__cold`, оборачивают C-функции, кидают через `error::success_or_throw`
(или `boolean_or_throw` для bool-результатов). Инфраструктура файла: `trouble_location`
(диагностика line/condition/function/file), `format_va`/`format`, класс `bug` +
`raise_bug`/`RAISE_BUG`, переопределение `ENSURE` для C++ (бросок `bug` вместо panic);
`fatal_countdown` для фатальных исключений.

| Класс | Неинлайн-методы |
| --- | --- |
| `exception`, `fatal` | конструкторы/деструкторы |
| `error` | `what()`, `message()`, `throw_exception()` (switch по `code()`) |
| `slice` | `is_printable(bool disable_utf8)` |
| `env` | `is_pristine()`, `is_empty()`, `copy(...)` (fd/const char*/std::string/W-перегрузки/`MDBX_STD_FILESYSTEM_PATH`), `get_path()`, `remove(...)` (перегрузки), `setup(max_maps,max_readers)`, open-фабрики (`make_parameters`/`create_parameters`, W-варианты) |
| `txn` | `start_nested()`, `start_nested(bool)`, `drop_map`/`clear_map`/`rename_map` (name/slice/string) |
| `txn_managed` | `commit_embark_read(finalization_latency*)` |
| `cursor` | `clone(context)`, `update_current(slice)`, `reverse_current(size_t)` |
| `cursor_managed` | `close()` (→ `mdbx_cursor_close2`) |

## 4. Мост C ↔ C++ (инварианты рефакторинга)

1. Каждый C++-метод — тонкая обёртка над `mdbx_*` C-функцией; хендлы хранятся в полях
   (`handle_`), managed-классы реализуют RAII.
2. Ошибки: `error::success_or_throw(rc)` / `error::boolean_or_throw(rc)` /
   `error::throw_on_nullptr(...)`; `error::throw_exception()` диспетчеризует по коду.
3. C-колбэки (предикаты сканирования, enumerate_readers, HSR) не могут бросать через C —
   обёртка `exception_thunk` ловит `std::exception_ptr` и возвращает код ошибки в C.
4. Wide-char/файловые перегрузки: Windows-ветки (`#if IS_WINDOWS`, `mdbx_env_copyW`,
   `mdbx_env_get_pathW`, `path_char`) и `MDBX_STD_FILESYSTEM_PATH`; GNUmakefile пробует
   поддержку filesystem через `cxx_filesystem_probe` и линкует `LIB_STDCXXFS`.
5. Ограничение безопасности: `!__LCC__` (Elbrus) — workaround «private field not used» в
   `bug::location()`; в шапке зафиксированы баги компиляторов (MSVC 19.2x optimizer hang,
   AppleClang без concepts).

## 5. Сборка и амальгамация

- Флаг `MDBX_BUILD_CXX`; стандарт выбирается цепочкой C++23→20→17→14→11 (`MDBX_CXX_STANDARD`,
  см. [`build.md`](build.md) §2.4). `src/mdbx.c++` требует `MDBX_BUILD_CXX=1` (иначе `#error`).
- GNUmakefile: `mdbx++-static.o`/`mdbx++-dylib.o` собираются из `src/mdbx.c++`;
  тестовый бинарник `mdbx_test` принудительно `-DMDBX_BUILD_CXX=1` (framework на C++).
- Амальгамация: `dist/mdbx.h++` = `mdbx.h++` с инлайном всех `mdbx++/*.h++` по маркерам
  `#include "mdbx++/..."` (см. [`build.md`](build.md) §7.5); каждый `mdbx++/*.h++` обёрнут в
  `// > dist-cutoff-begin/end` вокруг `#pragma once` и скобок `namespace mdbx {`.
- CTest: C++-тесты регистрируются в ветках `if(MDBX_BUILD_CXX)` (`tests/CMakeLists.txt`,
  `tests/issues/CMakeLists.txt`); `tests/framework/CMakeLists.txt` собирает `mdbx_test` из
  C++-исходников.

## 6. Покрытие тестами C++ API

- `tests/ut/*.c++` — большинство юнит-тестов написаны на C++ и используют публичный C++ API
  (`dbi.c++`, `txn.c++`, `open.c++`, `cursor_closing.c++`, `dupfix_multiple.c++`, ...).
- `tests/issues/issue_gh00XX.c++` — C++ регрессии.
- `examples/example-mdbx.c++` — «modern example» (собирается только при `MDBX_BUILD_CXX`).
- Сам фреймворк `mdbx_test` — C++ (см. [`build.md`](build.md) §6.1).
- ВНИМАНИЕ (пробел): явного отдельного набора тестов именно для C++-обёрток
  (slice/buffer/transcoders/exceptions-семантика) нет — `tests/ut/hex_base64_base58.c++`
  частично закрывает транскодеры. При рефакторинге C++-слоя это стоит расширять
  (см. цель по расширению быстрых юнит-тестов).

## 7. Матрица компиляторов (шапка mdbx.h++)

| Компилятор | С 2026 | До 2026 |
| --- | --- | --- |
| Elbrus LCC | ≥ 1.28 | ≥ 1.23 |
| GNU C++ | ≥ 11.3 | ≥ 4.8 |
| CLANG | ≥ 14.0 | ≥ 3.9 |
| MSVC | ≥ 19.44 (VS2022 v143) | ≥ 14.0 (VS2015); 19.2x — баг оптимизатора (зависание) |
| AppleClang | — | без C++20 concepts |

## 8. Рекомендации для рефакторинга

1. Не менять порядок `decl_* → impl_*` включений и forward-объявления в `begin.h++`.
2. «Горячие» пути (CRUD через `txn`/`cursor`) — инлайн в заголовках; вынос в
   `src/mdbx.c++` уместен только для `__cold`/исключительных веток (паттерн сохранён).
3. Новые wide-char/filesystem-перегрузки — следовать существующему паттерну
   `#if IS_WINDOWS` + `MDBX_STD_FILESYSTEM_PATH` + пробник filesystem.
4. Любые изменения шаблонных частей (`buffer`, `scan_until`) увеличивают время сборки —
   проверять уровень 1-2 на обоих CI-компиляторах (gcc/clang) и Windows (MSVC).
5. `MDBX_DECLARE_EXCEPTION`-семейство и `error::throw_exception()` — единая точка
   диспетчеризации ошибок; расширять аккуратно (поведенческий ABI).
6. При добавлении тестов на C++-обёртки — регистрировать в `if(MDBX_BUILD_CXX)` ветках и
   помнить про `add_extra_test`/`add_issue_test` макросы.