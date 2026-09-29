# libmdbx — внутренняя документация для разработчиков

> Внутренняя (developer-facing) документация по устройству и доработкам libmdbx.
> Перенесена из архива `poc1-failed:skynet/` и сверяется с актуальным кодом `master`.
> Публичная пользовательская документация живёт в `docs/` (Doxygen-сайт, см. `docs/_toc.md`).

## Документы

| Документ | Содержание |
| --- | --- |
| [`architecture.md`](architecture.md) | Внутренняя архитектура ядра: слои, ключевые структуры, MVCC/транзакции, конвейер коммита, состояния страниц, модель конкурентности, инварианты. |
| [`functional-architecture.md`](functional-architecture.md) | Функциональный взгляд «как это работает» без привязки к файлам: B+tree/MVCC/CoW, мета-тройка, GC, overflow, sync-режимы, RLT/HSR, парковка, WAF. [EN](functional-architecture.en.md) |
| [`libmdbx-improvements.md`](libmdbx-improvements.md) | Каталог доработок над LMDB по областям: надёжность, транзакции, GC, долговечность, B+tree, API, утилиты, оптимизации, сборка. [EN](libmdbx-improvements.en.md) |
| [`module-interfaces.md`](module-interfaces.md) | Карта межмодульных интерфейсов: экспорт из `src/proto.h` и модульных заголовков, include backbone, циклы зависимостей. |
| [`structure-review.md`](structure-review.md) | Аудит структуры репозитория (фаза A, TASK-36): карта каталогов, находки, исполненные кандидаты C1–C3. |
| [`debugging-methodology.md`](debugging-methodology.md) | Методики отладки: инструменты (`mdbx_chk`, PROFGC, commit_latency, ASAN/UBSAN/TSAN), пошаговые сценарии, внутренняя отладка. |
| [`deep-dive.md`](deep-dive.md) | Дополнения по пробелам покрытия: LCK-layout/версии, RLT-слоты, HSR-протокол, fork, WRITEMAP/авто-sync, восстановление. |
| [`verification.md`](verification.md) | Журнал сверки документов с кодом `master@f957a778`: подтверждённые факты с `file:line`, внесённые исправления. |

## Статус сверки

Документы перенесены из ветки `poc1-failed` (`skynet/`) и проходят сверку с
актуальным кодом `master`. Состояние каждого документа отмечается в его шапке
(`Верифицировано на master@<commit>`). Подробности — [`verification.md`](verification.md).

Неперенесённые спутники (остались в `poc1-failed:skynet/`): `structure.md`,
`build.md`, `test-coverage.md`, `cxx-api.md`, `techdebt.md`, `probes.md`,
`testing-infra-design.md`, `tools-rewrite/*`.