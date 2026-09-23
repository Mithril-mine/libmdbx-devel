# SKILL: Platform/Build Engineer (Level 3)

> Роль PB — сборка и CI на всех платформах. Настраивает конвейеры так, чтобы
> тесты были быстрыми (RAM-транспорт, safe_nosync). Знает платформенные
> особенности libmdbx.
> Обязательный контекст: `skills/shared/test-durability-strategy/SKILL.md`,
> `skills/shared/libmdbx-invariants/SKILL.md` (amalgamation).

## Identity & Mission

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
BUILD-RESULT: task-id, agent, status, platform, build-type, tests-run/passed,
  duration, transport, amalgamation, observations
```

## LTO (Link-Time Optimization) — справочник

### Что это и что даёт

LTO переносит межмодульную оптимизацию на этап линковки: компилятор сохраняет
GIMPLE-представление (gcc) / LLVM-IR (clang) в объектные файлы, линкер собирает
весь код и оптимизирует его целиком.

Плюсы:
- инлайн функций между translation units (например, hot-пути в одном TU,
  используемые из другого);
- точный alias/escape-анализ для констант и указателей;
- elimination неиспользуемых статических функций/данных (`-fwhole-program`);
- dead-code elimination на границах библиотека↔приложение;
- у clang/gcc — `-flto=auto` распараллеливает фазы (`lto1`/`lto-wrapper`).

Минусы:
- заметно дольше сборка и линковка (у libmdbx в Release ~50x на тестовых целях:
  каждый тест заново «переоптимизирует» всю библиотеку);
- больше RAM при линковке;
- диагностика тяжелее (номера строк смещаются, стектрейсы в gdb хуже);
- совместимость: нужен совместимый ar/ranlib/nm с LTO-плагином
  (gcc-`gcc-ar`, clang-`llvm-ar`), иначе «не видит» объекты;
- флейки отдельных компиляторов (gcc <9 — ложные -Wlto-type-mismatch;
  mscl — отдельная настройка `/GL`+`/LTCG`).

### Политика проекта (owner, B21)

- **LTO в ТЕСТОВЫХ сборках запрещён**, кроме явной необходимости: тестовые
  build-диры конфигурировать с `-DINTERPROCEDURAL_OPTIMIZATION=OFF`.
- LTO остаётся ON для release/library/tools сборок, где требуется.
- Проверять свои build-диры: `grep INTERPROCEDURAL_OPTIMIZATION CMakeCache.txt`
  — в тестовом билде должно быть `OFF`.

### Как управляется в CMake (libmdbx)

Цепочка: `cmake/compiler.cmake` детектит поддержку (GCC/CLANG/MSVC LTO + плагины),
выставляет `*_LTO_AVAILABLE`; затем в `CMakeLists.txt`:

- `INTERPROCEDURAL_OPTIMIZATION_DEFAULT` = ON для не-Debug сборок, если поддержка есть;
- `option(INTERPROCEDURAL_OPTIMIZATION ...)` — ключ управления;
- при ON: `set(LTO_ENABLED TRUE)`, подменяются `CMAKE_AR/RANLIB/NM` (+compiler-версии)
  на LTO-совместимые, выставляется `CMAKE_LINKER` (lld/ld);
- `cmake/compiler.cmake` по `LTO_ENABLED` добавляет глобально `-flto=auto
  -fno-fat-lto-objects -fuse-linker-plugin` (C;CXX), MSVC — `/GL`; линковочные
  флаги (`-fwhole-program -fverbose-asm` для EXE).
- Осторожно: флаги ГЛОБАЛЬНЫЕ (`add_compile_flags`) — затрагивают ВСЕ цели
  (библиотека, инструменты, каждый тест). Поэтому для тестов обязательно
  `-DINTERPROCEDURAL_OPTIMIZATION=OFF` на этапе конфигурации.
- Проверка включения: лог конфигурации «MDBX indulge Link-Time Optimization by …».

### Чек-лист CI-guru

1. Тестовый билд без LTO: `cmake -B <dir> -DINTERPROCEDURAL_OPTIMIZATION=OFF ...`.
2. Release/library/tools: LTO можно оставить по умолчанию (ON), если он нужен
   для производительности доставки.
3. В отчёте указывать wall-clock и факт LTO (`LTO=off/on`).
4. Amalgamation-gate не влияет на LTO, но LTO+amalgamated проверяются вместе
   на «доставку».

## Anti-Patterns

- Тесты на диске без необходимости.
- Без amalgamation-gate — broken build после merge.
- Игнор Windows single-process сжатия.
- Silent GTEST_SKIP на Android.
- Тестовый build-дир с LTO ON (медленно в ~50x; тестируем код, не компилятор).
- «Чинить» LTO в CMake-логике — LTO работает, отключаем только в тестах.