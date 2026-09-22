# SKILL: Swarm Management (Level 4, оркестратор)

> Управление пулом агентов и рабочими песочницами. Ресурсы хоста ограничены
> (4 vCPU / 15 GiB): пул busy ≤ 3 + оркестратор (протокол §25a).

## Пул ролей и паттерны активации

- Реестр ролей — `skynet_registry` (? агентов: ?, docs, vision, mailman[decommissioned], main).
- Одновременно активны **busy ≤ 3**; спящие роли хранят канонический ses_id и
  не плодят сессии.
- Активация/деактивация: switch-in = восстановить сессию (`-s`), синхронизировать
  с репозиторием, проверить module-lock, передать context-package;
  switch-out = завершить таск/checkpoint, сохранить контекст, освободить lock.
- Max 1 mid-sprint switch; агент неактивный >3 спринтов → STALE → re-warm.

## Nook-пул (консолидация, v2.13)

- 5 общих песочниц `nook-pool-1..5` (hardlink-шеринг `.git`); роль НЕ привязана
  к каталогу. Оркестратор назначает свободный nook (`nook_map` в state),
  переключает на ветку роли (`prepare_pool_nook`) и возобновляет каноническую
  сессию `-s --dir <pool>`.
- WIP-лимит по модулям: task-package содержит MODULE-LOCK; два таска с пересечением
  идут последовательно. Матрица конфликтов — `skills/shared/references/module-lock-matrix.md`
  (HIGH/MED/LOW). HIGH = sequential; MED = координация через constraints;
  LOW = параллельно безопасно. В amalgamated сборке lock ≈ file-lock.

## Context-switching (роль ↔ nook)

1. fetch local-origin; checkout ветки роли (last_ref из registry).
2. Возобновить `src code -- run -s <ses_id> --dir nook-pool-N`.
3. Дерево обязано быть чистым между сессиями (delivery-contract §22).

## Cross-role взаимодействие

TE→CR тесты; RE→CR код; RE→TE characterisation-запрос; FD→CR код; FD→TE тесты;
PB→CR pipeline; CR→все вердикт; все→DS decision logs; все→оркестратор OUTPUT+observations.
Ограничение: если CR в тройке — ≤2 задач требующих ревью за спринт.