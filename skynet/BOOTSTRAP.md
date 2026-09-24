# BOOTSTRAP — точка входа в знания о проекте libmdbx

> Внутренний документ роя/координатора. Точка входа и индекс знаний:
> где что лежит, как читать, что актуально. Создан 2026-09-24.
> Зеркало (вне git): `${SKYNET_ROOT}/BOOTSTRAP.md` — сверять при
> восстановлении контекста.

## 0. Назначение и место в системе

- Кто читает: координатор после потери/смены контекста, новый агент,
  владелец при обзоре.
- Что даёт: карту всех хранилищ знаний + порядок чтения + архитектурный
  срез «до» (референс для рефакторинга C→C++).
- Чем НЕ является: не живёт статусами задач и версиями (это BACKLOG /
  COORDINATION-STATE); обновляется только при структурных изменениях (§5).

## 1. Карта проекта (коротко)

**libmdbx** — встраиваемая транзакционная key-value БД (B+tree, MVCC + copy-on-write,
memory-mapped, no WAL / без crash-recovery), потомок LMDB, Apache-2.0,
C11 (движок) + опциональный C++ API (`mdbx.h++`). Автор — Леонид Юрьев.

- Канонический origin: **SourceCraft** `dqdkfa/libmdbx-devel`;
  GitHub — зеркало полной (devel) версии: `Mithril-mine/libmdbx-devel`.
  Отдельно есть `Mithril-mine/libmdbx` — amalgamated-версия для пользователей
  (НЕ путать; issues/PR ведём в `libmdbx-devel`).
- Модель веток: `working → devel → master → stable → lts`.
- Ключевые пути: `src/` (ядро ~60 модулей), `mdbx.h`/`mdbx.h++` (публичный API),
  `tests/` (framework/ut/issues), `skynet/` (доки/SKILLs), `cmake/`, `GNUmakefile`.
- Сборка/тесты: CMake (основной) + CTest; GNU Make legacy; Linux+Windows обязательны,
  macOS/ARM — по необходимости.
- Планируется глубокий рефакторинг C→C++ (фаза 1 — внутренности при стабильном
  C-API; фаза 2 — расширение API). Пилот — утилиты (B46).

## 1a. Внешние коммуникации и бэкпорты (факты, не требующие повторных объяснений)

- **Issues живут на GitHub devel-mirror** `Mithril-mine/libmdbx-devel`; наши
  внутренние номера (в BACKLOG/отчётах) = GitHub-номера. На SourceCraft issues нет;
  `Mithril-mine/libmdbx` (amalgamated) — пользовательский репозиторий.
- **Как комментировать issues** (issues-pr-codex §2): от имени skynet/Concordia
  Intellectuum, на языке пользователя; реакции 👀→🚀; финальный отчёт в треде с
  commit/backport-ссылками; закрытие issue после merge. В bug-репортах указывать
  файлы/номера строк (Kaizen владельца).
- **Бэкпорт**: только в **stable** и **lts/0.13** (master — фаза разработки v0.15.0,
  НЕ бэкпортим). Порядок: сначала полностью одна ветка → push → CI идёт, пока
  доделываешь вторую.
- **devel-merge НЕ пушим без команды владельца** (не занимать CI); пуш наружу —
  только после локальных проверок и решения владельца.
- Методика саморевью при единственном исполнителе: `.skynet/REANIMATION-PLAN.md` §2.

## 2. Хранилища знаний

| Хранилище | Что там | Как читать |
|---|---|---|
| `skynet/README.md` | проектный индекс (карта репо, навигация) | первым делом |
| `skynet/build.md` | сборка, конфигурация, тестирование, амальгама | для build/test команд |
| `skynet/structure.md` | файл-за-файлом карта `src/` | для модулей |
| `skynet/architecture.md` | внутренняя архитектура, инварианты | для движка |
| `skynet/functional-architecture.md` | «как работает» (функц. взгляд, RU) | для понимания механик |
| `skynet/module-interfaces.md` | экспорт/зависимости модулей | для интерфейсов |
| `skynet/cxx-api.md` | C++ API слой в деталях | для mdbx.h++ |
| `skynet/test-coverage.md` | какие тесты защищают какие модули | для рефакторинга |
| `skynet/techdebt.md` | 86 TODO/FIXME/workaround | для кандидатов на правку |
| `skynet/news-codex.md`, `issues-pr-codex.md`, `changelog-codex.md` | кодексы коммуникаций | для внешнего общения |
| `skynet/skills/README.md` | иерархия SKILLs L0–L4 | для ролей/навыков |
| `skynet/skynet-protocol.md` | протокол координации агентов (рой) | для работы роя |
| `.skynet/BACKLOG.md` | бэклог B1..B63 | статусы задач |
| `.skynet/COORDINATION-STATE.md` | живой журнал координации | текущее состояние |
| `.skynet/OPERATING-MODE.md` | текущий режим работы (ECONOMY §7) | режим роя |
| MCP-memory | граф знаний `~/.local/share/libmdbx-memory/graph.mdbx` | `memory_mdbx_search_nodes` |
| git `knowledge` | база знаний/опыта: `ref/state/experience` | `git fetch origin knowledge` |

## 3. Порядок чтения для целей

- **Восстановление после потери контекста**: `BOOTSTRAP.md` → `COORDINATION-STATE.md`
  → `BACKLOG.md` → `skynet/README.md` → devel `git log` → memory-MCP (при необходимости).
- **Работа с продуктом (build/test)**: `skynet/README.md` §5 quick start → `build.md`.
- **Понимание движка**: `functional-architecture.md` → `architecture.md` → `structure.md`.
- **Рефакторинг C→C++**: §4 (срез «до») → `test-coverage.md` → characterization-тесты
  (пилот B46) → по модулям.
- **Работа с роем** (когда рой снова активен): `skynet-protocol.md` → `skills-roles.md`
  → `skynet/skills/orchestrator/*`.

## 4. Архитектурный срез «до» (референс для рефакторинга)

> Срез на момент создания документа: **2026-09-24T~21:10Z**. Не живой раздел —
> актуалку смотреть в COORDINATION-STATE/BACKLOG.

- **devel**: локальный лидер `65cd5099`; upstream отстал (`45129461` = Merge B54);
  github отстал (`ccd74bb3`). Рассинхронизация — известно, синхронизировать по
  решению владельца.
- **Недожатый P0 — #49** (`mdbx_chk -i` segfault на именованных под-таблицах):
  фикс готов (`fix/issue49-chk-cookie@639a884b`, 2 коммита) + backport-ветки
  (master/stable/lts), валидировано — **нет REV:c и merge в devel**.
- **P1 открытые**: #53 (MSBuild ntdll_extra race, ci-windows-msvc red),
  MACOSGOLD (macos golden-failure, ci-macos red).
- **Известные CI-флейки/red**: macos-golden, #53, B13 (smoke_fault timeout),
  B14 (ARM64 smoke_sp) — подробности в BACKLOG.
- **Ключевые инварианты движка** (беречь при переписке): MVCC + CoW, no WAL /
  crash-recovery-free; wait-free параллельные читатели, один писатель; B+tree
  страницы (dpl/pnl/rkl); GC (gc-get/gc-put); mmap-модель (writemap);
  osal по платформам; **амальгама всегда зелёная** (`make dist`, dist-cutoff,
  build.md §7).
- **Режим работы**: ECONOMY — рой остановлен (оркестратор off), агентов
  НЕ запускать без явной команды владельца (OPERATING-MODE §7).

## 5. Правило обновления (ленивое, дешёвое)

Триггеры обновления — ТОЛЬКО структурные события:
1. Добавлено новое верхнеуровневое хранилище/раздел знаний.
2. Изменилась карта репозитория (крупные переносы каталогов).
3. Сменился режим работы роя (ECONOMY ↔ норма) или протокол координации.
4. Начался/завершился этап рефакторинга C→C++ (фаза 1/2, пилот B46).
5. Выполнена реструктуризация MCP-памяти (по решению владельца).

НЕ триггеры: обычные коммиты, статусы задач, merge'и, версии, содержимое
BACKLOG/COORDINATION-STATE (они меняются сами, bootstrap на них ссылается).

Механика: при структурном изменении — дописать/поправить соответствующую строку
§2/§4 сразу, в том же коммите (чек-лист «изменилась карта знаний → sync BOOTSTRAP»).
Цель — обновление почти бесплатно, документ никогда не расходится с реальностью
в главном, но не требует ресурсов на каждом шаге.