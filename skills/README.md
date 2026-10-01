# Skills — каталог навыков и профилей

> Навыки существуют независимо от какой-либо организации агентов: это активные
> методики работы над libmdbx, доступные через `skill` tool (см. `.opencode/skills/`).
> Организационные артефакты (роли, протокол роя) живут отдельно — в ветке `skynet`.

## Структура

| Каталог | Назначение |
|---|---|
| [`profiles/`](profiles/) | **Профили** — набор навыков под конкретный тип задачи (выбирай под задачу): `developer`, `refactoring`, `testing`, `code-review`, `platform-build`, `documentation` |
| [`shared/`](shared/) | Общие навыки (для всех профилей): `memory-hygiene`, `security-boundary`, `kaizen-principles`, `references/approach-bank.md` |
| [`superpowers/`](superpowers/) | Интегрированная методология разработки (адаптация obra/superpowers, MIT): brainstorming, writing-plans, TDD, systematic-debugging и др. |
| [`research/`](research/) | Исследования и материалы: `swarm-coordination-research.md` |
| [`cpp/`](cpp/) | Навык Modern C++ (C++20/23/26) для libmdbx (MIT) |
| [`refactor/`](refactor/) | Навык безопасного рефакторинга (Fowler-каталог, MIT) |
| [`rules-skill-writer/`](rules-skill-writer/) | Методология написания собственных навыков |

Справочники об устройстве libmdbx (инварианты, подсистемы S1–S14, матрицы,
режимы тестов) — в [`docs/engineering/`](../docs/engineering/):
[`libmdbx-invariants.md`](../docs/engineering/libmdbx-invariants.md),
[`subsystem-map.md`](../docs/engineering/subsystem-map.md),
[`module-lock-matrix.md`](../docs/engineering/module-lock-matrix.md),
[`test-durability-strategy.md`](../docs/engineering/test-durability-strategy.md).

## Как выбирать профиль

| Задача | Профиль |
|---|---|
| Новая функция API / оптимизация WAF | [`developer`](profiles/developer/SKILL.md) |
| Рефакторинг B+tree/CoW/GC/API | [`refactoring`](profiles/refactoring/SKILL.md) |
| Декомпозиция тестов, characterisation, CI-pipeline | [`testing`](profiles/testing/SKILL.md) |
| Ревью изменений (C/CXX/ARCH) | [`code-review`](profiles/code-review/SKILL.md) |
| Сборка, CI, amalgamation, NDK, LTO | [`platform-build`](profiles/platform-build/SKILL.md) |
| Поддержка документации, ADR | [`documentation`](profiles/documentation/SKILL.md) |

Общий поток любой разработки — методология
[`superpowers/README.md`](superpowers/README.md): brainstorm → plan → worktree →
TDD → review → finish.

## Правила активации

1. Выбери профиль под задачу (таблица выше) и загрузи его через `skill` tool.
2. Обязательный контекст профиля — справочники `docs/engineering/*` (инварианты,
   матрицы, стратегии) — читай по ссылкам, не дублируй содержимое.
3. Адаптируй навык под контекст: ссылки на пути — относительные, от `skills/`.