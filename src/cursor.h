/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

/* Состояние курсора.
 *
 * плохой/poor:
 *  - неустановленный курсор с незаполненым стеком;
 *  - следует пропускать во всех циклах отслеживания/корректировки
 *    позиций курсоров;
 *  - допускаются только операции предполагающие установку абсолютной позиции;
 *  - в остальных случаях возвращается MDBX_ENODATA.
 *
 *    У таких курсоров top = -1 и flags < 0, что позволяет дешево проверять и
 *    пропускать такие курсоры в циклах отслеживания/корректировки по условию
 *    probe_cursor->top < this_cursor->top.
 *
 * пустой/hollow:
 *  - частично инициализированный курсор, но без доступной пользователю позиции,
 *    поэтому нельзя выполнить какую-либо операцию без абсолютного (не
 *    относительного) позиционирования;
 *  - ki[top] может быть некорректным, в том числе >= page_numkeys(pg[top]).
 *
 *    У таких курсоров top >= 0, но flags < 0 (есть флажок z_hollow).
 *
 * установленный/pointed:
 *  - полностью инициализированный курсор с конкретной позицией с данными;
 *  - можно прочитать текущую строку, удалить её, либо выполнить
 *    относительное перемещение;
 *  - может иметь флажки z_after_delete, z_eof_hard и z_eof_soft;
 *  - наличие z_eof_soft означает что курсор перемещен за пределы данных,
 *    поэтому нелья прочитать текущие данные, либо удалить их.
 *
 *    У таких курсоров top >= 0 и flags >= 0 (нет флажка z_hollow).
 *
 * наполненный данными/filled:
 *  - это установленный/pointed курсор без флагов z_eof_soft;
 *  - за курсором есть даные, возможны CRUD операции в текущей позиции.
 *
 *    У таких курсоров top >= 0 и (unsigned)flags < z_eof_soft.
 *
 * Изменения состояния.
 *
 *  - Сбрасывается состояние курсора посредством combo_state |= z_poor_mark,
 *    что равносильно top = -1 вместе с flags |= z_poor_mark;
 *  - При позиционировании курсора сначала устанавливается top, а flags
 *    только в самом конце при отсутстви ошибок.
 *  - Повторное позиционирование first/last может начинаться
 *    с установки/обнуления только top без сброса flags, что позволяет работать
 *    быстрому пути внутри tree_search().
 *
 *  - Заморочки с концом данных:
 *     - mdbx_cursor_get(NEXT) выполняет две операции (перемещение и чтение),
 *       поэтому перемещение на последнюю строку строку всегда успешно,
 *       а ошибка возвращается только при последующем next().
 *       Однако, из-за этой двойственности семантика ситуации возврата ошибки
 *       из mdbx_cursor_get(NEXT) допускает разночтение/неопределенность, ибо
 *       не понятно к чему относится ошибка:
 *        - Если к чтению данных, то курсор перемещен и стоит после последней
 *          строки. Соответственно, чтение в текущей позиции запрещено,
 *          а при выполнении prev() курсор вернется на последнюю строку;
 *        - Если же ошибка относится к перемещению, то курсор не перемещен и
 *          остается на последней строке. Соответственно, чтение в текущей
 *          позиции допустимо, а при выполнении prev() курсор встанет
 *          на пред-последнюю строку.
 *        - Пикантность в том, что пользователи (так или иначе) полагаются
 *          на оба варианта поведения, при этом конечно ожидают что после
 *          ошибки MDBX_NEXT функция mdbx_cursor_eof() будет возвращать true.
 *     - далее добавляется схожая ситуация с MDBX_GET_RANGE, MDBX_LOWERBOUND,
 *       MDBX_GET_BOTH_RANGE и MDBX_UPPERBOUND. Тут при неуспехе поиска курсор
 *       может/должен стоять после последней строки.
 *     - далее добавляется MDBX_LAST. Тут курсор должен стоять на последней
 *       строке и допускать чтение в текузщей позиции,
 *       но mdbx_cursor_eof() должен возвращать true.
 *
 *    Решение = делаем два флажка z_eof_soft и z_eof_hard:
 *     - Когда установлен только z_eof_soft,
 *       функция mdbx_cursor_eof() возвращает true, но допускается
 *       чтение данных в текущей позиции, а prev() передвигает курсор
 *       на пред-последнюю строку.
 *     - Когда установлен z_eof_hard, чтение данных в текущей позиции
 *       не допускается, и mdbx_cursor_eof() также возвращает true,
 *       а prev() устанавливает курсора на последюю строку. */

/* Кодирование состояния: контракт зон младшего байта flags (менять нельзя —
 * зафиксировано STATIC_ASSERT-ом после enum).
 *
 * Значения битов образуют 4 монотонные зоны, каждая определяет класс
 * обработки курсора; два порога — z_after_delete (0x08) и z_eof_hard (0x40) —
 * совпадают с границами классов:
 *
 *   0x00..0x07  | 0x08..0x3F  | 0x40..0x7F | 0x80..0xFF / combo_state < 0
 *   Z0 plain    | Z1 edge     | Z2 eof     | Z3 hollow/poor
 *
 *  - Z0 plain: позиция на данных, без отложенных эффектов — fastpath,
 *    прямой доступ к pg[top]/ki[top] без проверок состояния
 *    (is_filled подразумевается);
 *  - Z1 edge: краевое, но читаемое и CRUD-способное — z_after_delete
 *    (позиция перенесена на следующий элемент), z_inner_tail_deleted
 *    (хвост мультизначений удалён), z_eof_soft (физически на последней
 *    строке);
 *  - Z2 eof: логически за концом данных, CRUD запрещён;
 *  - Z3 hollow/poor: позиция не определена или курсор сброшен.
 *
 * Разделение fastpath/slowpath — одно сравнение классификационного байта
 * cursor_state_byte() (см. ниже): fastpath <=> байт < z_after_delete.
 *
 * Младший байт занят полностью (8 флагов). Новые флаги размещаются только
 * в битах старше 7-го: байт остаётся 8-битным для бэкпорта в stable
 * и lts/0.13, а односравнительный гейт зон не ломается. */
enum cursor_state {
  /* Это вложенный курсор для вложенного дерева/страницы и является
     inner-элементом struct cursor_couple. */
  z_inner = 0x01,

  /* Происходит подготовка к обновлению GC,
     поэтому можно брать страницы из GC даже для FREE_DBI. */
  z_gcu_preparation = 0x02,

  /* Курсор только-что создан, поэтому допускается авто-установка
     в начало/конец, вместо возврата ошибки. */
  z_fresh = 0x04,

  /* Предыдущей операцией было удаление, поэтому курсор уже физически указывает
     на следующий элемент и соответствующая операция перемещения должна
     игнорироваться. */
  z_after_delete = 0x08,

  /* Неиспользуемая заготовка, ранее занимавшая бит 0x10: не была реализована
     ни в путях поиска, ни в путях обработки; присваивание в walk_tbl() было
     лишь напоминанием и не влияло на поведение. При возобновлении этой
     функциональности использовать биты старше 7-го. */
  /* z_disable_tree_search_fastpath = 0x10, */

  /* (внутренний) курсор логически стоит на «хвосте» удалённых мультизначений:
     позиция за последним значением ключа после удаления последнего значения.
     Повторное удаление в этом состоянии легально и является no-op,
     а GET_CURRENT/NEXT обязаны вернуть следующую запись. */
  z_inner_tail_deleted = 0x10,

  /* Курсор логически в конце данных, но физически на последней строке,
     ki[top] == page_numkeys(pg[top]) - 1 и может читать данные в текущей позиции. */
  z_eof_soft = 0x20,

  /* Курсор логически за концом данных, поэтому следующий переход "назад"
     должен игнорироваться и/или приводить к установке на последнюю строку.
     В текущем же состоянии нельзя делать CRUD операции. */
  z_eof_hard = 0x40,

  /* За курсором нет данных, логически его позиция не определена,
     нельзя делать CRUD операции в текущей позиции.
     Относительное перемещение запрещено. */
  z_hollow = -128 /* 0x80 */,

  /* Маски для сброса/установки состояния. */
  z_clear_mask = z_inner | z_gcu_preparation,
  /* Сброс состояния: одна запись отрицательного combo_state (top = stash = -1).
     Бит z_inner_tail_deleted не включается: сброс не должен claim'ить
     краевые состояния Z1. */
  z_poor_mark = z_eof_hard | z_hollow,
  z_fresh_mark = z_poor_mark | z_fresh
};

/* Контракт зон кодирования состояния (см. комментарий перед enum): значения
 * и порядок битов зафиксированы, так как пороги z_after_delete/z_eof_hard
 * задают границы классов обработки, а z_hollow — трюк сброса одной записью. */
STATIC_ASSERT(z_after_delete == 0x08 && z_inner_tail_deleted == 0x10 && z_eof_soft == 0x20 && z_eof_hard == 0x40 &&
              z_hollow == -128);

MDBX_MAYBE_UNUSED static inline bool has_inner(const MDBX_cursor *mc) { return mc->subcur != nullptr; }

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool is_inner(const MDBX_cursor *mc) {
  return (mc->flags & z_inner) != 0;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool is_poor(const MDBX_cursor *mc) {
  const bool r = mc->top < 0;
  cASSERT0(mc, r == (mc->combo_state < 0));
  if (r && has_inner(mc))
    cASSERT0(mc, mc->subcur->cursor.flags < 0 && mc->subcur->cursor.top < 0);
  return r;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool is_pointed(const MDBX_cursor *mc) {
  const bool r = mc->top >= 0;
  cASSERT0(mc, r == (mc->combo_state >= 0));
  if (!r && has_inner(mc))
    cASSERT0(mc, is_poor(&mc->subcur->cursor));
  return r;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool is_hollow(const MDBX_cursor *mc) {
  const bool r = mc->flags < 0;
  if (!r) {
    cASSERT0(mc, mc->top >= 0);
    cASSERT0(mc, (mc->flags & z_eof_hard) || mc->ki[mc->top] < page_numkeys(mc->pg[mc->top]));
  } else if (has_inner(mc))
    cASSERT0(mc, is_poor(&mc->subcur->cursor) || (is_pointed(mc) && mc->subcur->cursor.flags < 0));
  return r;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool is_eof(const MDBX_cursor *mc) {
  const bool r = z_eof_soft <= (uint8_t)mc->flags;
  return r;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool is_filled(const MDBX_cursor *mc) {
  const bool r = z_eof_hard > (uint8_t)mc->flags;
  return r;
}

/* Классификационный байт состояния: младший байт flags + знак top (бит 23
 * combo_state) в бите 7. Одно сравнение решает всё:
 *   <  z_after_delete — fastpath: Z0 plain, проверки состояния не нужны;
 *   >= z_after_delete — slowpath: краевое/eof/hollow/poor состояние.
 * Знак top покрывает транзитные состояния перепозиционирования, в которых
 * установлен только top, а байт flags выглядит plain. */
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline uint8_t cursor_state_byte(const MDBX_cursor *mc) {
  return (uint8_t)mc->flags | (uint8_t)(((uint32_t)mc->combo_state >> 16) & 0x80);
}

/* Для couple: OR внешнего и внутреннего до сравнения — та же одна условная
 * ветвь на весь fast/slow. */
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline uint8_t couple_state_byte(const cursor_couple_t *couple) {
  return cursor_state_byte(&couple->outer) | cursor_state_byte(&couple->inner.cursor);
}

/* Сбрасывает z_eof_hard только когда курсор стоит на элементе; z_eof_soft
 * сохраняется — он документирует «логически в конце, но читаемо в текущей
 * позиции». Позиция за хвостом (ki == page_numkeys) обязана сохранить
 * z_eof_hard, иначе она читалась бы как заполненная и CRUD-операции уходили
 * бы за пределы данных (issue #305). Курсор с маркером хвоста удалённых
 * мультизначений не очищается вовсе: иначе после отвергнутого NEXT_DUP
 * повторное удаление перестало бы быть no-op и удалило бы реальное значение
 * (issue #301). */
MDBX_MAYBE_UNUSED static inline void eof_hard_clear(MDBX_cursor *mc) {
  if (!(mc->flags & z_inner_tail_deleted) && is_pointed(mc) && mc->ki[mc->top] < page_numkeys(mc->pg[mc->top]))
    mc->flags &= ~z_eof_hard;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool inner_filled(const MDBX_cursor *mc) {
  return has_inner(mc) && is_filled(&mc->subcur->cursor);
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool inner_pointed(const MDBX_cursor *mc) {
  return has_inner(mc) && is_pointed(&mc->subcur->cursor);
}

/* (внутренний) курсор логически стоит на «хвосте» удалённых мультизначений:
 * позиция за последним значением ключа. Guard по маске отличает это
 * состояние от сброшенного курсора (z_poor_mark не включает этот бит) и от
 * позиции с отложенным z_after_delete (логическая позиция уже перенесена на
 * следующий элемент — no-op хвоста не должен срабатывать для продвинутой
 * позиции). */
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool inner_tail_deleted(const MDBX_cursor *mc) {
  return (mc->flags & (z_hollow | z_after_delete | z_inner_tail_deleted)) == z_inner_tail_deleted;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool inner_hollow(const MDBX_cursor *mc) {
  const bool r = !has_inner(mc) || is_hollow(&mc->subcur->cursor);
#if MDBX_CHECKING > 0
  if (!r) {
    cASSERT0(mc, is_filled(mc));
    const page_t *mp = mc->pg[mc->top];
    const node_t *node = page_node(mp, mc->ki[mc->top]);
    cASSERT0(mc, node_flags(node) & N_DUP);
  }
#endif /* MDBX_CHECKING > 0 */
  return r;
}

MDBX_MAYBE_UNUSED static inline void inner_gone_unconditional(MDBX_cursor *mc) {
  TRACE("reset inner cursor %p", __Wpedantic_format_voidptr(&mc->subcur->cursor));
  cASSERT0(mc, !is_inner(mc));
  cursor_couple_t *couple = container_of(mc, cursor_couple_t, outer);
  couple->inner.nested_tree.root = 0;
  couple->inner.cursor.combo_state = z_inner | z_poor_mark;
}

MDBX_MAYBE_UNUSED static inline void inner_gone(MDBX_cursor *mc) {
  if (has_inner(mc)) {
    TRACE("reset inner cursor %p", __Wpedantic_format_voidptr(&mc->subcur->cursor));
    cursor_couple_t *couple = container_of(mc, cursor_couple_t, outer);
    couple->inner.nested_tree.root = 0;
    couple->inner.cursor.combo_state = z_inner | z_poor_mark;
  }
}

MDBX_MAYBE_UNUSED static inline void be_poor(MDBX_cursor *mc) {
  const bool inner = is_inner(mc);
  if (inner) {
    mc->tree->root = 0;
    mc->combo_state = z_inner | z_poor_mark;
  } else {
    mc->combo_state |= z_poor_mark;
    inner_gone_unconditional(mc);
  }
  cASSERT0(mc, is_poor(mc) && !is_pointed(mc) && !is_filled(mc));
  cASSERT0(mc, inner == is_inner(mc));
}

MDBX_MAYBE_UNUSED static inline void be_filled(MDBX_cursor *mc) {
  cASSERT0(mc, mc->top >= 0 && mc->stash == 0);
  cASSERT0(mc, mc->ki[mc->top] < page_numkeys(mc->pg[mc->top]));
  const bool inner = is_inner(mc);
  mc->flags &= z_clear_mask;
  cASSERT0(mc, is_filled(mc));
  cASSERT0(mc, inner == is_inner(mc));
}

MDBX_MAYBE_UNUSED static inline bool is_related_include(const MDBX_cursor *base, const MDBX_cursor *scan) {
  cASSERT0(base, base->top >= 0);
  return base->top <= scan->top;
}

MDBX_MAYBE_UNUSED static inline bool is_related(const MDBX_cursor *base, const MDBX_cursor *scan) {
  return is_related_include(base, scan) && base != scan;
}

/* Флаги контроля/проверки курсора. */
enum cursor_checking {
  z_branch = 0x01 /* same as P_BRANCH for check_leaf_type() */,
  z_leaf = 0x02 /* same as P_LEAF for check_leaf_type() */,
  z_largepage = 0x04 /* same as P_LARGE for check_leaf_type() */,
  z_updating = 0x08 /* update/rebalance pending */,
  z_ignord = 0x10 /* don't check keys ordering */,
  z_dupfix = 0x20 /* same as P_DUPFIX for check_leaf_type() */,
  z_retiring = 0x40 /* refs to child pages may be invalid */,
  z_pagecheck = 0x80 /* perform page checking, see MDBX_VALIDATION */
};

MDBX_INTERNAL int __must_check_result cursor_validate(const MDBX_cursor *mc);
MDBX_INTERNAL int __must_check_result cursor_validate_updating(MDBX_cursor *mc);
MDBX_MAYBE_UNUSED MDBX_INTERNAL void cursor_stack(const MDBX_cursor *const mc, const char *func, unsigned line,
                                                  const char *prefix);

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline size_t cursor_dbi(const MDBX_cursor *mc) {
  cASSERT0(mc, mc->txn->signature == txn_signature);
  size_t dbi = mc->dbi_state - mc->txn->dbi_state;
  cASSERT0(mc, dbi < mc->txn->env->n_dbi);
  return dbi;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool cursor_dbi_changed(const MDBX_cursor *mc) {
  return dbi_changed(mc->txn, cursor_dbi(mc));
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline uint8_t *cursor_dbi_state(const MDBX_cursor *mc) {
  return mc->dbi_state;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool cursor_is_gc(const MDBX_cursor *mc) {
  return mc->dbi_state == mc->txn->dbi_state + FREE_DBI;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool cursor_is_main(const MDBX_cursor *mc) {
  return mc->dbi_state == mc->txn->dbi_state + MAIN_DBI;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool cursor_is_core(const MDBX_cursor *mc) {
  return mc->dbi_state < mc->txn->dbi_state + CORE_DBS;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline MDBX_cursor *cursor_outer(MDBX_cursor *mc) {
  cASSERT0(mc, mc->flags & z_inner);
  return &container_of(mc, cursor_couple_t, inner.cursor)->outer;
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline tree_t *cursor_outer_tree(MDBX_cursor *mc) {
  cASSERT0(mc, (mc->flags & z_inner) != 0);
  subcur_t *mx = container_of(mc->tree, subcur_t, nested_tree);
  cursor_couple_t *couple = container_of(mx, cursor_couple_t, inner);
  cASSERT0(mc, mc->tree == &couple->outer.subcur->nested_tree && &mc->clc->k == &couple->outer.clc->v);
  return couple->outer.tree;
}

MDBX_MAYBE_UNUSED static inline int cursor_dbi_dbg(const MDBX_cursor *mc) {
  /* Debugging output value of a cursor's DBI: Negative for a sub-cursor. */
  const int dbi = cursor_dbi(mc);
  return (mc->flags & z_inner) ? -dbi : dbi;
}

MDBX_MAYBE_UNUSED static inline int __must_check_result cursor_push(MDBX_cursor *mc, page_t *mp, indx_t ki) {
  TRACE("pushing page %" PRIaPGNO " on db %d cursor %p", mp->pgno, cursor_dbi_dbg(mc), __Wpedantic_format_voidptr(mc));
  cASSERT0(mc, mc->top >= -1);
  size_t new_top = mc->top + 1;
  if (unlikely(new_top >= CURSOR_STACK_SIZE)) {
    be_poor(mc);
    mc->txn->flags |= MDBX_TXN_ERROR;
    return MDBX_CURSOR_FULL;
  }
  mc->top_and_stash = (int16_t)new_top;
  cASSERT0(mc, mc->top == (int)new_top && mc->stash == 0);
  mc->pg[new_top] = mp;
  mc->ki[new_top] = ki;
  return MDBX_SUCCESS;
}

MDBX_MAYBE_UNUSED static inline void cursor_eject(MDBX_cursor *mc) {
  TRACE("eject page %" PRIaPGNO " off db %d cursor %p", mc->pg[mc->top]->pgno, cursor_dbi_dbg(mc),
        __Wpedantic_format_voidptr(mc));
  cASSERT0(mc, mc->top >= 0 && mc->stash == 0);
  mc->top_and_stash = mc->top - 1;
}

MDBX_MAYBE_UNUSED static inline void cursor_enroot(MDBX_cursor *mc, int step) {
  TRACE("cursor %p (dbi %i) enroot %+i to %i, pgno %u", __Wpedantic_format_voidptr(mc), cursor_dbi_dbg(mc), step,
        mc->top - step, (mc->top >= step) ? mc->pg[mc->top - step]->pgno : 0);
  cASSERT0(mc, mc->top >= step && mc->stash >= 0);
  const int prev_stash = mc->stash, prev_top = mc->top;
  int16_t delta = (step << CHAR_BIT) - step;
  mc->top_and_stash += delta;
  ASSERT(mc->top >= 0 && mc->top == prev_top - step && mc->stash > 0 && mc->stash == prev_stash + step);
}

MDBX_MAYBE_UNUSED static inline void cursor_undo_enroot(MDBX_cursor *mc, int step) {
  TRACE("cursor %p (dbi %i) undo %+i, to %i, pgno %u", __Wpedantic_format_voidptr(mc), cursor_dbi_dbg(mc), step,
        mc->top + step, mc->pg[mc->top + step]->pgno);
  const int prev_stash = mc->stash, prev_top = mc->top;
  int16_t delta = (step << CHAR_BIT) - step;
  mc->top_and_stash -= delta;
  ASSERT(mc->top > 0 && mc->top == prev_top + step && mc->stash >= 0 && mc->stash == prev_stash - step);
}

MDBX_NOTHROW_PURE_FUNCTION static inline bool check_leaf_type(const MDBX_cursor *mc, const page_t *mp) {
  return (((page_type(mp) ^ mc->checking) & (z_branch | z_leaf | z_largepage | z_dupfix)) == 0);
}

MDBX_INTERNAL int cursor_check(const MDBX_cursor *mc, int txn_bad_bits);

/* без необходимости доступа к данным, без активации припаркованных транзакций. */
static inline int cursor_check_pure(const MDBX_cursor *mc) {
  return cursor_check(mc, MDBX_TXN_BLOCKED - MDBX_TXN_PARKED);
}

/* для чтения данных, с активацией припаркованных транзакций. */
static inline int cursor_check_ro(const MDBX_cursor *mc) { return cursor_check(mc, MDBX_TXN_BLOCKED); }

/* для записи данных. */
static inline int cursor_check_rw(const MDBX_cursor *mc) {
  return cursor_check(mc, (MDBX_TXN_BLOCKED - MDBX_TXN_PARKED) | txn_ro_both);
}

MDBX_INTERNAL MDBX_cursor *cursor_eot(MDBX_cursor *cursor, MDBX_txn *txn);
MDBX_INTERNAL int cursor_shadow(MDBX_cursor *cursor, MDBX_txn *nested, const size_t dbi);

MDBX_INTERNAL MDBX_cursor *cursor_cpstk(const MDBX_cursor *csrc, MDBX_cursor *cdst);
MDBX_INTERNAL MDBX_cursor *cursor_copy_position(const MDBX_cursor *csrc, MDBX_cursor *cdst);

MDBX_INTERNAL int __must_check_result cursor_ops(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data,
                                                 const MDBX_cursor_op op);

MDBX_INTERNAL int __must_check_result cursor_check_multiple(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data,
                                                            unsigned flags);

MDBX_INTERNAL int __must_check_result cursor_put_checklen(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data,
                                                          unsigned flags);

MDBX_INTERNAL int __must_check_result cursor_put(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data, unsigned flags);

MDBX_INTERNAL int __must_check_result cursor_del(MDBX_cursor *mc, unsigned flags);

MDBX_INTERNAL int __must_check_result cursor_sibling_left(MDBX_cursor *mc);
MDBX_INTERNAL int __must_check_result cursor_sibling_right(MDBX_cursor *mc);

typedef struct cursor_set_result {
  int err;
  bool exact;
} csr_t;

MDBX_INTERNAL csr_t cursor_seek(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data, MDBX_cursor_op op);

MDBX_INTERNAL int __must_check_result inner_first(MDBX_cursor *__restrict mc, MDBX_val *__restrict data);
MDBX_INTERNAL int __must_check_result inner_last(MDBX_cursor *__restrict mc, MDBX_val *__restrict data);
MDBX_INTERNAL int __must_check_result outer_first(MDBX_cursor *__restrict mc, MDBX_val *__restrict key,
                                                  MDBX_val *__restrict data);
MDBX_INTERNAL int __must_check_result outer_last(MDBX_cursor *__restrict mc, MDBX_val *__restrict key,
                                                 MDBX_val *__restrict data);

MDBX_INTERNAL int __must_check_result inner_next(MDBX_cursor *__restrict mc, MDBX_val *__restrict data);
MDBX_INTERNAL int __must_check_result inner_prev(MDBX_cursor *__restrict mc, MDBX_val *__restrict data);
MDBX_INTERNAL int __must_check_result outer_next(MDBX_cursor *__restrict mc, MDBX_val *__restrict key,
                                                 MDBX_val *__restrict data, MDBX_cursor_op op);
MDBX_INTERNAL int __must_check_result outer_prev(MDBX_cursor *__restrict mc, MDBX_val *__restrict key,
                                                 MDBX_val *__restrict data, MDBX_cursor_op op);

MDBX_INTERNAL int cursor_init4walk(cursor_couple_t *couple, const MDBX_txn *const txn, tree_t *const tree,
                                   kvx_t *const kvx);

MDBX_INTERNAL int __must_check_result cursor_init(MDBX_cursor *mc, const MDBX_txn *txn, size_t dbi);

MDBX_INTERNAL int __must_check_result cursor_dupsort_setup(MDBX_cursor *mc, const node_t *node, const page_t *mp);

MDBX_INTERNAL int __must_check_result cursor_touch(MDBX_cursor *const mc, const MDBX_val *key, const MDBX_val *data);

MDBX_INTERNAL MDBX_cursor *cursor_clone_share_inner_tree(const MDBX_cursor *csrc, cursor_couple_t *couple);
MDBX_MAYBE_UNUSED MDBX_INTERNAL MDBX_cursor *cursor_clone_complete(const MDBX_cursor *csrc, cursor_couple_t *couple);

MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL int cursor_on_first(const MDBX_cursor *mc);
MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL int cursor_on_last(const MDBX_cursor *mc);
MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL intptr_t cursor_cmp(const MDBX_cursor *left, const MDBX_cursor *right);

MDBX_INTERNAL int cursor_distribute(const MDBX_cursor *begin, const MDBX_cursor *end, MDBX_cursor **array,
                                    size_t array_size, int level);
MDBX_INTERNAL int cursor_scroll_forward(MDBX_cursor *mc, intptr_t amount, int level);
MDBX_INTERNAL int cursor_scroll_backward(MDBX_cursor *mc, intptr_t amount, int level);

typedef struct cursor_distance_result {
  int err;
  size_t distance;
} cdr_t;
MDBX_INTERNAL cdr_t cursor_distance(MDBX_cursor *begin, const MDBX_cursor *end, int level);

/*----------------------------------------------------------------------------*/

/* Update sub-page pointer, if any, in mc->subcur.
 * Needed when the node which contains the sub-page may have moved.
 * Called with mp = mc->pg[mc->top], ki = mc->ki[mc->top]. */
MDBX_MAYBE_UNUSED static inline void cursor_inner_refresh(const MDBX_cursor *mc, const page_t *mp, unsigned ki) {
  cASSERT0(mc, is_leaf(mp));
  const node_t *node = page_node(mp, ki);
  if ((node_flags(node) & (N_DUP | N_TREE)) == N_DUP)
    mc->subcur->cursor.pg[0] = node_data(node);
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline bool
outer_on_duptree_and_inner_pointed(const MDBX_cursor *mc) {
  if (inner_pointed(mc)) {
    size_t whole = mc->top + mc->stash;
    cASSERT0(mc, mc->top >= 0 && mc->stash >= 0 && whole < CURSOR_STACK_SIZE && whole < mc->tree->height);
    const page_t *mp = mc->pg[whole];
    if (is_leaf(mp) && mc->ki[whole] < page_numkeys(mp))
      return F_ISSET(page_node(mp, mc->ki[whole])->flags, N_DUP | N_TREE);
  }
  return false;
}

MDBX_MAYBE_UNUSED MDBX_INTERNAL bool cursor_is_tracked(const MDBX_cursor *mc);

static inline void cursor_reset(cursor_couple_t *couple) {
  couple->outer.combo_state = z_fresh_mark;
  couple->inner.cursor.combo_state = z_fresh_mark | z_inner;
  couple->inner.nested_tree.root = 0;
}

static inline void cursor_drown(cursor_couple_t *couple) {
  couple->outer.combo_state = z_poor_mark;
  couple->inner.cursor.combo_state = z_poor_mark | z_inner;
  couple->inner.nested_tree.root = 0;
  couple->outer.txn = nullptr;
  couple->inner.cursor.txn = nullptr;
  couple->outer.tree = nullptr;
  /* сохраняем clc-указатель, так он используется для вычисления dbi в mdbx_cursor_renew(). */
  couple->outer.dbi_state = nullptr;
  couple->inner.cursor.dbi_state = nullptr;
}
