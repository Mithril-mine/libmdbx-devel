# Module Lock Matrix — матрица конфликтов

> Reference (Level 2, `shared/references/`). Часть иерархии SKILLS, см.
> `skynet/skills/README.md`. Источник: `structure.md` (§2/§7),
> `module-interfaces.md`, `libmdbx-invariants.md` (подсистемы S1–S14).
> Назначение: распределение module-locks при параллельной работе агентов.

## 1. Правила (по swarm-management)

- **HIGH** = task-package содержит MODULE-LOCK; два таска с пересечением выполняются
  **последовательно** (WIP-лимит по модулям).
- **MED** = координация через constraints; допускается параллельность с оговорками.
- **LOW** = параллельно безопасно.
- В amalgamated сборке lock ≈ file-lock.

## 2. Матрица подсистем (пары с конфликтом)

| Подсистема A | Подсистема B | Уровень | Комментарий |
|---|---|---|---|
| S1 API | S2 B+tree | MED | API вызывает дерево; сигнатуры тронуть можно, но параллельный рефакторинг обоих рискован |
| S1 API | S6 Txn | MED | api-txn* тесно связаны с txn.c |
| S2 B+tree | S3 CoW | 🔴 HIGH | node/dml/page-ops — одна зона изменений |
| S2 B+tree | S11 Cursor | 🔴 HIGH | cursor.c реализует поиск по дереву |
| S3 CoW | S6 Txn | 🔴 HIGH | dirtylist/spilled/refund в `MDBX_txn.wr` |
| S4 GC | S5 Meta | MED | gc-put обновляет мету через txn path |
| S4 GC | S6 Txn | 🔴 HIGH | GC участвует в commit pipeline |
| S4 GC | S9 Parallel | MED | rkl/RTL разделяют файл блокировок |
| S5 Meta | S8 Durability | 🔴 HIGH | двухфазное обновление меты + sync |
| S5 Meta | S10 Growth | MED | dxb resize трогает геометрию |
| S6 Txn | S9 Parallel | MED | rthc/lck связаны с txn жизненным циклом |
| S6 Txn | S11 Cursor | MED | txn_commit шедоуит курсоры (`txn_done_cursors`) |
| S7 Overflow | S4 GC | MED | bigfoot/overflow страницы переиспользуются GC |
| S8 Durability | S10 Growth | MED | fsync при resize |
| S9 Parallel | S12 Service | LOW | chk/backup читают без блокировок |
| S11 Cursor | S13 Get-cached | MED | оба читающие пути к dbi |
| S13 Get-cached | S14 WAF | LOW | независимые оптимизации |
| S14 WAF | S2 B+tree | MED | rebalance/merge на дереве |

## 3. Модульный уровень (src/*)

Конкретные файлы — зоны с наивысшим риском (из `structure.md` §8):

| Файл/зона | Причина | Параллельные задачи |
|---|---|---|
| `internals.h` (`MDBX_txn`) | центральная структура, union `wr` | НЕ делить: любой refactor = HIGH |
| `cursor.c` / `cursor.h` | 4-состоятельная машина, z_eof | HIGH с tree/dml |
| `page-ops.h` | page state machine | HIGH с dml/txl/spill |
| `proto.h` | индекс «кто что экспортирует» | любое добавление функции = MED |
| `api-get-cached.c` | 7 статусов, ABA | MED с api-cursor |
| `lck-windows.c` / `lck-posix.c` | платформенные бэкенды | LOW между собой (разные OS) |
| `options.h` ↔ `conanfile.py` | зеркальные опции | MED: менять согласованно |
| `src/mdbx.c++` / `mdbx++/` | C++-слой | MED с S1 C API |

## 4. Как пользоваться

1. При декомпозиции задачи оркестратор проставляет `MODULE-LOCK: <модуль>` в
   task-package (см. swarm-management).
2. Два active таска с пересечением → HIGH: второй ждёт завершения первого (sequential);
   MED: разнести по коммитам/констрейнтам; LOW: параллельно.
3. Освобождение — наблюдение `claim=<область>=<released>|<ts>` (§15 протокола).
4. Проверять актуальность карты при изменении `structure.md` (Weekly Codex Review).

> Синхронизировать с `subsystem-map.md` и `libmdbx-invariants.md` (S-таблица).