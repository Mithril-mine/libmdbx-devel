# SKILLS и роли коллективной работы над libmdbx (канон)

> Реестр ролей: **`X.роль`**, где `X` — стабильный уникальный код (0–9, A–Z),
> не меняется при смене роли; `X` используется в почте/inbox, сессиях, задачах.
> Канон согласован с владельцем 2026-09-22 (после чистки неверной картины
> «Desktop/Mobile / Windows/macOS как разные роли»).
> Источники: `nook-owner-dont-touch/libmdbx_skills_roles.md` (краткий канон),
> L3 role-SKILL-файлы (расширенное описание). `agent_roles_skill.md` —
> УСТАРЕВШИЙ (заражён платформенными специализациями).
> Миссия союза «Concordia Intellectuum»: `skynet/MISSION.md`.

## Роли и исполнители

| X | Роль | Зона ответственности | SKILL (L3) | Статус |
|---|---|---|---|---|
| `0` | Оркестратор / Координатор | целостность архитектуры, синхронизация, приоритизация, зависимости, CI/CD, метрики | L4 `skills/orchestrator/*` | активен (бывш. main_architect) |
| `A` | testcase-guru | пишет тестовые сценарии, декомпозиция монолитов, characterisation-тесты | `test-engineer/SKILL.md` | активен (бывш. tests_worker) |
| `B` | tester | пишет и выполняет тесты, edge-case, stress/fuzz, восстановление после сбоев | `test-engineer/SKILL.md` | активен (бывш. tests_writer) |
| `G` | tester | пишет и выполняет тесты (вторая инстанция роли, разгрузка пула) | `test-engineer/SKILL.md` | новая (2026-09-23) |
| `C` | ci-guru | сборка, CI/CD конвейеры, amalgamation, решение проблем CI на всех платформах | `platform-build-engineer/SKILL.md` | активен (бывш. sysprobe) |
| `D` | reviewer-c | код-ревью C-ядра: CoW, B+tree, GC, meta, курсоры, mmap | `code-reviewer/SKILL.md` | активен (бывш. review_cmake) |
| `E` | reviewer-cxx | код-ревью C++ API: RAII, исключения, типобезопасность | `code-reviewer/SKILL.md` | активен (бывш. review_cpp) |
| `F` | reviewer-cmake | код-ревью CMake/сборки/воркфлоу CI | `code-reviewer/SKILL.md` + `platform-build-engineer/SKILL.md` | активен (бывш. review_win) |
| `S` | scribe | документация, doxygen, ADR, версионирование, API-референс | `documentation-scribe/SKILL.md` | активен (бывш. docs) |
| `I` | developer-cxx | расширение C++ API, RAII-обёртки, интеграция с C API | (использует `code-reviewer` базово; роль из `libmdbx_skills_roles.md`) | свободна |
| `J` | refactoring-cxx | глубокий рефакторинг C++-слоя под защитой characterisation-тестов | `refactoring-engineer/SKILL.md` | свободна |
| `L` | refactoring-core | рефакторинг B+tree/CoW/курсоров (S2/S3/S11) | `refactoring-engineer/SKILL.md` | свободна |
| `M` | refactoring-gc | рефакторинг GC/детент/bigfoot (S4) | `refactoring-engineer/SKILL.md` | свободна |
| `N` | refactoring-api | C→C++ migration, ABI (S1) | `refactoring-engineer/SKILL.md` | свободна |
| `O` | feature-api | новые функции API, get-cached | `feature-developer/SKILL.md` | свободна |
| `P` | feature-engine | WAF-оптимизации, GC-тюнинг | `feature-developer/SKILL.md` | свободна |
| `R` | researcher | поиск информации, исследование архитектуры/поведения, проверенные выводы | (базовый) | свободна |
| `T` | tooling | разработка инструментов (telegram-мост, вспомогательный инструментарий) | (базовый) | свободна |
| `V` | vision | генерация идей, дивергенция; вызывается по запросу | `skills/roles/vision/SKILL.md` (TASK-34) | эпизодическая |
| `_` | mailman | доставка писем waitmail/fifo/MCP; НЕ агент, компонент оркестратора | — | служебный |

### Схлопнутые id (не переиспользуются)

- `K` — была CI-специалист (слита в `C.ci-guru`).
- `Q` — был toolsmith (переименован в `T.tooling`).
- `H` — был ci-guru (переехал на `C`).

## Коллективные SKILLS (для всей команды)

- Git-workflow и работа с PR: разделение на логические коммиты, локальная валидация
  перед REPORT (контракт §27), CI-проверки до публикации.
- Коммуникация в технических деталях: отсылки к конкретным функциям
  («проблема с `MDBX_DUPSORT` при компактификации»), а не общие слова.
- Совместное планирование: короткие синхронизации, спорные решения — координатору.
- Фиксация знаний: память роя (memory-codex), обновление сущностей после работ,
  инварианты и «ловушки именований» с провенансом.
- Бережливость к ресурсам (Kaizen): сборки `nice 5`, тесты `nice 10`, один
  opencode-процесс на nook, без дублирующих прогонов (§27), адресный отбор тестов.

## Чек-лист ревью (доменный, базовый)

- [ ] Не нарушает ли изменение инварианты libmdbx?
- [ ] Корректно ли обрабатываются все коды возврата?
- [ ] Нет ли утечек ресурсов (транзакции, курсоры, mmap)?
- [ ] Не ухудшает ли изменение производительность (WAF, GC, tail latency)?
- [ ] Добавлены ли тесты для нового/изменённого кода? Учтён ли mdbx_chk?
- [ ] Обновлена ли документация (включая doxygen-чистоту)?
- [ ] Соблюдён ли стиль кодирования (LLVM)?
- [ ] Есть ли VALIDATED-матрица (§27)?

## Применение по направлениям

- **Рефакторинг**: декомпозиция по модулям (txn/GC/B-tree/multivalues/cursor/env),
  ответственный за модуль, координатор следит за инвариантами и совместимостью.
- **Тесты**: testcase-guru (A) пишет сценарии, tester (B) пишет и выполняет;
  адресное покрытие белых пятен (B17), stress/fuzz.
- **Документация**: scribe (S), разделы по модулям, синхронизация с кодом, ADR.
- **C++ API**: developer-cxx (I), RAII-обёртки + тесты, согласование с координатором.
- **Ревью**: маршрутизация веток по доменам (`REV:<domain>`, §28):
  `REV:c` → D.reviewer-c, `REV:cxx` → E.reviewer-cxx, `REV:cmake` → F.reviewer-cmake.