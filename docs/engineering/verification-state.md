/// \file verification-state.md
/// \brief Verification state of the codebase: what is proven, what to watch.

# Состояние верификации кодовой базы (по итогам кампании #485)

Итог кампании постанционной верификации
([#485](https://github.com/Mithril-mine/libmdbx-devel/issues/485), закрыта
2026-11-19): statement-level проверка 155/155 файлов, ~8 900 из 9 711
обязательств, 9 реальных дефектов найдено и исправлено (scan4seq-units-false-claim отозван как
ложный), ~48 прочих находок (контракты, документация, харденнинг,
производительность). Этот файл — компактное состояние «что доказано /
на что смотреть»; каталог доказанных инвариантов —
[`libmdbx-invariants.md`](libmdbx-invariants.md) §7, контракты примитивов —
[`module-interfaces.md`](module-interfaces.md) §6; опыт методик —
[`testing-methodology.md`](testing-methodology.md) глава 10. Носители
знания в shared-graph-memory: `fact:core:verification-state`,
`bug:issue485:final-rollup`, `proc:verification:methodology-effectiveness`.

## Что доказано хорошо (зоны без открытых дефектов)

| Зона | Основание |
|---|---|
| PNL/DPL/RKL/sort-ядро | леммы PNL-slack/capacity/order, DPL-cap/order, RKL-I1…I5, SORT-*; парный аудит всех call-site (PAIR-N1..N6) |
| Долговечность/мета/формат | двухфазный протокол меты, полнота meta_validate (14 ветвей), карта выходов dxb_sync_locked, two-phase shrink |
| Дерево (tree*.c) | транш дерева: ноль опровергнутых; LEM-V4-1..5, INV-V4-Z |
| Транзакционное ядро | финальная линза: ноль новых опровергнутых; INV-T1..T10 |
| GC (после фиксов gc-store-retired-growth-corruption/gc-dense-solver-unbounded-stall) | store-retired закрыт регресс-тестом; dense-solver заменён на закрытую форму (валидация против брутфорса и независимого DP-оракула) |
| Координация (S9) | INV-S9-1..S9-6, PAIR-N10/N11 |
| API/C++ | INV-CC/INV-E1/INV-CXX*, C++-обёртки zero-overhead |

Перформанс-негативы тоже результат: branchless-поиск, cmov-bsearch, DPL
42-window pivot, oldest-scan bound, scan4seq dispatch и др. — проверены
оптимальными (глава 10, §7.8 каталога инвариантов).

## На что смотреть дальше (хвосты кампании — заведённые issues)

1. **Windows RDONLY mapping growth** (#549) — два Windows-only пути,
   ни один не подтверждён рантаймом на Windows (major-if-confirmed):
   - `windows-lck-restored-copy-opener-race` — подмена lck-файла
     восстановленной копией в сочетании с гонкой двух открывателей может
     привести второй открыватель к shared-return seize без эксклюзивного
     dxb-лока → два мира координации → параллельные писатели (тихая
     порча); свежая подмена обезвреживается 2-секундным дедлайном;
   - `windows-readonly-mmap-extent-fixed-at-open` — экстент RDONLY-mapping
     в native-Windows фиксируется на момент открытия (NtCreateSection
     MaximumSize), тогда как POSIX отображает полный лимит: читатель,
     чей новый снапшот требует страниц за экстентом, получает EPERM в
     `mdbx_txn_begin`; RDONLY fast path (map->current = размер без
     расширения секции) недостижим через движок и unsound при достижении;
2. **fix-vs-docs по тройке контрактов defrag** (#550, issue #55) — нужно
   решение владельца «фикс или документируем»; тесты перевернутся по
   вердикту:
   - `defrag-stuck-retry-success-verdict` — stuck-retry стоп возвращает
     SUCCESS+no-obstacles; закреплён ut `defrag-contract` с маркером
     known-gap, воспроизведение детерминированное;
   - `defrag-time-limit-unreachable-goal` — time_limit игнорируется, когда
     цель недостижима (машина-проверено);
   - `defrag-adopted-txn-mid-commit-user-writes` — adopted-транзакция
     коммитит записи пользователя mid-txn (put переживает
     mdbx_txn_abort после defrag), недокументировано (машина-проверено);
3. **C++ API minors** (#551) — отложены:
   - `cxx-decoder-empty-source-predicate` — `from_hex`/`from_base64`:
     `is_erroneous()` отвергает источники без декодируемых символов,
     которые decode принимает (`is_hex("")==false`, `is_base58("")==true`)
     — несогласованный предикат пустого источника;
   - `cxx-is-printable-ascii-mask-aliasing` — ASCII-режим
     `slice::is_printable` принимает 0xE0–0xEF: `3<<LS` алиасит `P_` в
     маске (проверено исчерпывающе; соседние классы лидов корректны);
   - `cxx-map-handle-slice-minors` — семейство мелочей: b58 size-estimate
     32-битное переполнение для ~2 ГБ значений (недостижимо), out-of-range
     scoped-enum из комбинаций флагов в `map_handle::info`,
     `slice::assign` пишет base до проверки длины, noexcept-трейт
     `buffer_pair_spec` на патологических аллокаторах, доки
     `is_hex("")`/`try_*_reserve`, нетемплейтный `txn::replace` не
     трогает old_value на dirty-page старых значениях;
4. **Инфраструктура** (#552) — Windows RDONLY тест (зависит от #549);
   round-trip fuzz инфраструктура mdbx_dump/mdbx_load/mdbx_copy;
5. **chk-interrupt-exit-code** (#553) — SIGINT во время `mdbx_env_chk()`
   давал код 3 на master против 5 после фикса; единственный
   воспроизведённый дефект кампании без suite-теста (shell-probe only) —
   нужен ограниченный дизайн теста (тайминги сигнала).

Институционализировано в CI: UBSan gate-клетка (#530) ловит семейство
bounds-ловушек `root-split-cursor-stack-shift-bound`; compile-клетка
`MDBX_ENABLE_BUNCHES_REMOVAL=0` (`bunches-removal-off-build-break`);
SIMD differential (details_scan4seq) охраняет семейство scan4seq после
отзыва `scan4seq-units-false-claim`.

## Обязательства

- L3/troika-fsm-proof-unused: `troika_verify_fsm()` (машинное доказательство FSM, 216
  состояний) не вызывается нигде — активировать ut-тестом;
- покрытие: структурная карта и таблицы покрытия в shared-graph-memory
  обновляются CI-пайплайном (артефакты `coverage-<cell>`/
  `cell-metrics-<cell>`); белые пятна — `coverage_gaps` модуля.
