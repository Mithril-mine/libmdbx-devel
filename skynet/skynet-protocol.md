# skynet — протокол координации агентов (v1)

> Внутренний протокол взаимодействия агентов, работающих над **libmdbx-devel**
> на одной машине. Реализуется поверх общего хранилища **MCP-memory**
> (см. `AGENT-WORKSPACE.md`, раздел «Общая память агентов»).
> Не попадает в амальгамированную версию.
>
> Версия протокола: **1** (развивается, см. §8).

## 1. Цель и принципы

Агенты работают параллельно в собственных nook-ах (см. `AGENT-WORKSPACE.md`)
и координируются **асинхронно** через «почту» в MCP-memory. Почта — только
краткая конкретика (что, кому, когда, ссылка); весь объёмный контекст
передаётся файлами (в репозитории — коммиты/PR, вне репозитория —
`/sourcecraft/workspace/.skynet/`).

Три правила:

1. **Единый источник правды** — граф MCP-memory (общий для всех агентов).
2. **Асинхронность** — письма не требуют одновременного присутствия агентов;
   каждый агент читает почту при старте сессии и периодически.
3. **Восстановимость** — ситуацию можно понять по одним лишь данным памяти,
   либо по одним лишь git-состоянию (см. §7 «Восстановление»).

## 2. Сущности в MCP-memory

| Сущность | Назначение |
| --- | --- |
| `skynet` | мета: версия протокола, формат писем, ссылки на документ |
| `skynet_registry` | реестр агентов: roster, heartbeat-сводка, правила рангов/сессий |
| `skynet_agent_<slug>` | состояние агента: ранг, роль, специализация, status, session_id, heartbeat, last_ref |
| `skynet_inbox_<slug>` | почтовый ящик агента (наблюдения = письма) |

Связи (relations): `skynet_registry -tracks-> agent`,
`agent -uses_mailbox-> inbox`, `skynet -defines-> registry`.

Соглашения об именах: slug латиницей в нижнем регистре, слова через `_`
(`main_architect`, `tests_lead`, ...). Папка внешних артефактов:
`/sourcecraft/workspace/.skynet/` (вне git).

## 3. Ранги, роли, специализация

- **Ранг** (иерархия): `0` owner (человек), `1` главный архитектор/координатор,
  `2` ведущие специалисты (leads), `3` исполнители (workers).
- **Роль** (чем занимается): coordinator, architect, reviewer, tester, writer,
  skills-maintainer, release-manager (можно несколько, через запятую).
- **Специализация** (фокус): наполнение/поддержание SKILL и контекста
  по своему направлению (core/engine, cpp-api, tests/qa, build/ci, docs/legal...).

Правила изменений:

- Ранги в основном назначает **owner**. Командир может менять ранг/роли
  подчинённых **в пределах своей иерархии** (т.е. агент ранга N управляет
  рангами > N и ниже себя).
- **Специализацию** меняет только **owner или главный архитектор**.
- Изменения фиксируются наблюдением в `skynet_agent_<slug>`
  (`rank=...`, `role=...`, `specialization=...`) с датой и автором изменения.

## 4. Реестр и heartbeat

Каждый агент при старте сессии:

1. Генерирует `session_id` (uuid) и записывает его в свой
   `skynet_agent_<slug>` (наблюдение `session_id=...`).
2. Обновляет `status=active`, `heartbeat=<ISO-ts>`, `last_ref=<branch> <commit>`.
3. Сверяет себя с `skynet_registry`: если он там не значится — регистрируется
   (см. §5), иначе — сверяет session_id и heartbeat.

Периодически (во время долгих работ и в конце сессии) агент обновляет
heartbeat. Сводку ведёт главный архитектор/координатор в `skynet_registry`.

Правило устаревания (stale):

- Агент **stale**, если `heartbeat` старше 60 минут, **либо** в его записи
  `session_id` отличается от текущего `session_id` агента.
- Stale-агент при пробуждении обязан: перечитать память + git,
  выполнить процедуру восстановления (§7), перерегистрироваться, обновить
  session_id/heartbeat.
- Координатор не назначает задачу агенту со stale-статусом, пока тот не
  перерегистрируется.

## 5. Регистрация нового агента

Агент создаёт в MCP-memory:

- `skynet_agent_<slug>` (наблюдения: slug, name, rank, role, specialization,
  status, session_id, heartbeat, last_ref, mailbox).
- `skynet_inbox_<slug>` (наблюдение `empty at registration <ts>`).
- Связи: `skynet_registry -tracks-> agent`, `agent -uses_mailbox-> inbox`.
- Сообщение в `skynet_inbox_main_architect`: `NOTIFY "new agent <slug> registered"`.

Затем главный архитектор подтверждает регистрацию (`ACK`) и включает агента
в roster `skynet_registry`.

## 6. Почта (mailboxes)

### 6.1 Формат письма

Каждое письмо — наблюдение в `skynet_inbox_<получатель>`:

```
[<msg_id>|<ISO-ts>] <from> -> <to> : <TYPE> <subject> | payload: <path|ref> | <однострочное тело>
```

- `msg_id` — сквозной идентификатор: `mail-<seq>-<from>-<to>` (seq — счётчик отправителя).
- `TYPE` — одно из: `PING`, `HEARTBEAT`, `TASK`, `REQUEST`, `QUESTION`,
  `REPORT`, `NOTIFY`, `ACK`, `EOT`.
- `payload:` — ссылка на файл/коммит/PR (если есть). Тяжёлый контекст — только тут.
- Тело — одна строка, без вложенности.

Примеры:

```
[mail-1-main_architect-tests_lead|2026-09-20T11:20:00Z] main_architect -> tests_lead : TASK add cursor batch tests | payload: devel@c4e44373 | branch feature/cursor-batch from devel, target P1 ut.cursor
[mail-2-tests_lead-main_architect|2026-09-20T13:05:00Z] tests_lead -> main_architect : REPORT done | payload: local-origin feature/cursor-batch@f00d | pushed, 3 tests green on gcc+clang
[mail-3-main_architect-tests_lead|2026-09-20T13:07:00Z] main_architect -> tests_lead : ACK ok | merged to devel
```

### 6.2 Поток работы

- Отправитель кладёт письмо в ящик получателя и, при важности, уведомляет
  (`NOTIFY` в ящик координатора).
- Получатель при старте/в цикле читает свой ящик, обрабатывает письма,
  отвечает (`REPORT`/`QUESTION`/`ACK`) и **помечает обработанное** дописав
  `[done <ts>]` к исходному наблюдению (или удаляя его — по договорённости).
- Подтверждение задачи: `TASK` → ожидается `ACK` (принял) и `REPORT` (сделал)
  или `EOT` (завершил с итогом).

### 6.3 Файловый канал

Объёмный контекст — в файлы:

- В репозитории: ветки, коммиты, PR (указываются в `payload:`).
- Вне репозитория: `/sourcecraft/workspace/.skynet/<msg_id>/` (например,
  полные логи, диффы, отчёты). В git не коммитить.

## 7. Восстановление ситуации (важно)

Если что-то пошло не так, агент обязан восстановить картину **по любому
одному источнику**:

### Только данные MCP-memory (новая/старая копия)
- Прочитать `skynet_registry` (roster, heartbeat, stale-правило) →
  понять, кто активен/занят.
- Прочитать свой `skynet_inbox_<slug>` → восстановить незакрытые `TASK`
  (без `EOT`/`ACK`), на которые надо ответить.
- Прочитать `skynet` (версия протокола) и `skynet_agent_*` соседей.
- Если собственная запись имеет чужой/старый `session_id` → это «я после
  рестарта»; предыдущие in-flight задачи считать незавершёнными.

### Только git-состояние
- `git log`, `git status`, ветки в `local-origin` → восстановить, где остановилась
  работа (последний `last_ref` из памяти, ветка/коммит).
- Проверить CI-статусы (SourceCraft/GitHub) — что валидировано.
- Сверить с `skynet/README.md` и `AGENT-WORKSPACE.md` (структура и правила).

После восстановления агент синхронизирует память и git (обновляет
session_id, heartbeat, last_ref) и сообщает координатору.

## 8. Развитие протокола

- Изменения протокола — новым мажорным/минорным номером в `skynet_registry`
  (`protocol_version=...`) и правкой этого документа.
- Предложения: письмо `QUESTION "protocol: ..."` координатору (главному
  архитектору) → решение → правка документа → `NOTIFY "protocol vX.Y"`.

## 9. Текущие агенты

| slug | rank | role | specialization | status |
| --- | --- | --- | --- | --- |
| `main_architect` | 1 | coordinator, architect, reviewer | core engine, C++ API, tests/infrastructure, skynet coordination | active |

(roster поддерживается координатором в `skynet_registry`.)