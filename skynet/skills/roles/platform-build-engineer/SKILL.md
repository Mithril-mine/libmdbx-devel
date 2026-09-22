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

## Anti-Patterns

- Тесты на диске без необходимости.
- Без amalgamation-gate — broken build после merge.
- Игнор Windows single-process сжатия.
- Silent GTEST_SKIP на Android.