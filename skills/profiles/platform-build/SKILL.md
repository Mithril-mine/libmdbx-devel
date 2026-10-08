---
name: platform-build
description: Profile for building libmdbx and running CI on all platforms — CI-gate setup, amalgamation-gate, Android NDK, LTO policy, fast test transports.
---

# Platform/build profile — сборка и CI libmdbx

> Обязательный контекст: [`docs/engineering/test-durability-strategy.md`](../../../docs/engineering/test-durability-strategy.md),
> [`docs/engineering/libmdbx-invariants.md`](../../../docs/engineering/libmdbx-invariants.md) (amalgamation),
> [`docs/engineering/build.md`](../../../docs/engineering/build.md).

## Область

Сборка на всех платформах; настройка конвейеров так, чтобы тесты были быстрыми
(RAM-транспорт, safe_nosync). Знает платформенные особенности libmdbx.

Linux/Windows/macOS/Android сборка; amalgamation-gate; CI: SourceCraft (primary),
GitHub Actions (mirror).

## Task Protocols

### Настройка CI-gate
По платформам: prerequisites (build-essential/systemtap-sdt-dev, MSVC, clang+dtrace,
NDK), transport (TMPDIR=/dev/shm, ImDisk RAM-диск/fallback-уменьшение, /tmp,
неприменимо), CTest Quick/Full + таймауты; amalgamation-gate; reporting; mirror.

### Amalgamation-gate
Запустить amalgamate.py → собрать amalgamated → тесты → PASS? нет REJECT.
Если структурных изменений нет → SKIP с отметкой.

### Android NDK
NDK+Gradle; кросс-компиляция CMake toolchain (arm64-v8a/armeabi-v7a/x86_64);
только SAFE_NOSYNC; USDT недоступен → GTEST_SKIP с причиной; уменьшенный объём;
CI best-effort experimental.

## Output Format

```
BUILD-RESULT: task-id, status, platform, build-type, tests-run/passed,
  duration, transport, amalgamation, observations
```

## LTO (Link-Time Optimization) — краткий справочник

LTO переносит межмодульную оптимизацию на этап линковки (GIMPLE/LLVM-IR в
объектных файлах, линкер оптимизирует всё вместе).

Плюсы: инлайн между translation units, точный alias-анализ, dead-code elimination,
`-flto=auto` распараллеливает фазы. Минусы: заметно дольше сборка (~50x на
тестовых целях libmdbx: каждый тест заново «переоптимизирует» всю библиотеку),
больше RAM, тяжелее диагностика, нужны LTO-совместимые ar/ranlib/nm, флейки
отдельных компиляторов (gcc <9, mscl `/GL`+`/LTCG`).

### Политика проекта (B21)
- **LTO в ТЕСТОВЫХ сборках запрещён**: конфигурировать с `-DINTERPROCEDURAL_OPTIMIZATION=OFF`.
- LTO остаётся ON для release/library/tools сборок, где требуется.
- Проверять: `grep INTERPROCEDURAL_OPTIMIZATION CMakeCache.txt` — в тестовом
  билде должно быть `OFF`.

### Как управляется в CMake (libmdbx)
Цепочка: `cmake/compiler.cmake` детектит поддержку (GCC/CLANG/MSVC LTO + плагины),
выставляет `*_LTO_AVAILABLE`; в `CMakeLists.txt`:
- `INTERPROCEDURAL_OPTIMIZATION_DEFAULT` = ON для не-Debug сборок;
- `option(INTERPROCEDURAL_OPTIMIZATION ...)` — ключ управления;
- при ON: подменяются `CMAKE_AR/RANLIB/NM`, выставляется `CMAKE_LINKER`;
- флаги ГЛОБАЛЬНЫЕ (`add_compile_flags`) — затрагивают ВСЕ цели, поэтому для
  тестов обязательно `-DINTERPROCEDURAL_OPTIMIZATION=OFF` на конфигурации.

### Чек-лист
1. Тестовый билд без LTO: `cmake -B <dir> -DINTERPROCEDURAL_OPTIMIZATION=OFF ...`.
2. Release/library/tools: LTO можно оставить по умолчанию (ON).
3. В отчёте указывать wall-clock и факт LTO (`LTO=off/on`).
4. Amalgamation-gate не влияет на LTO, но LTO+amalgamated проверяются вместе.

## Anti-Patterns

- Тесты на диске без необходимости.
- Без amalgamation-gate — broken build после merge.
- Игнор Windows single-process сжатия.
- Silent GTEST_SKIP на Android.
- Тестовый build-дир с LTO ON (медленно в ~50x; тестируем код, не компилятор).
- «Чинить» LTO в CMake-логике — LTO работает, отключаем только в тестах.