/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file cursor-nav.c — cursor movement/navigation: sibling page moves,
/// the bring/brim/step position-templates wrappers (outer_first/last/next/prev
/// and inner_*), on_first/on_last position predicates and long-range
/// scrolling; part of the cursor.c module (see also cursor-setup.c /
/// cursor-mut.c / cursor-seek.c and cursor-internal.h).

#include "internals.h"
#include "cursor-internal.h"


__hot int cursor_sibling_left(MDBX_cursor *mc) {
  int err = sibling(mc, false);
  if (likely(err != MDBX_NOTFOUND))
    return err;

  cASSERT0(mc, mc->top >= 0);
  size_t nkeys = page_numkeys(mc->pg[mc->top]);
  cASSERT0(mc, nkeys > 0);
  mc->ki[mc->top] = 0;
  /* не очищаем состояние вложенного курсора, а остаёмся на предыдущей позиции */
  return MDBX_NOTFOUND;
}

__hot int cursor_sibling_right(MDBX_cursor *mc) {
  int err = sibling(mc, true);
  if (likely(err != MDBX_NOTFOUND))
    return err;

  cASSERT0(mc, mc->top >= 0);
  size_t nkeys = page_numkeys(mc->pg[mc->top]);
  cASSERT0(mc, nkeys > 0);
  mc->ki[mc->top] = (indx_t)nkeys - 1;
  mc->flags = z_eof_soft | z_eof_hard | (mc->flags & z_clear_mask);
  /* не очищаем состояние вложенного курсора, а остаёмся на предыдущей позиции */
  return MDBX_NOTFOUND;
}

intptr_t cursor_cmp(const MDBX_cursor *left, const MDBX_cursor *right) {
  ASSERT((is_inner(left) != 0) == (is_inner(right) != 0));
  ASSERT(cursor_dbi(left) == cursor_dbi(right));
  ASSERT(left->top == right->top);
  intptr_t cmp = 0;
  for (intptr_t i = 0; i <= left->top; ++i) {
    ASSERT(left->pg[i] == right->pg[i]);
    cmp = (intptr_t)left->ki[i] - (intptr_t)right->ki[i];
    if (cmp)
      break;
  }
  return cmp;
}

/*----------------------------------------------------------------------------*/

/* Функция-шаблон: Приземляет курсор на данные в текущей позиции.
 * В том числе, загружает данные во вложенный курсор при его наличии. */
/* Функция-шаблон: Устанавливает курсор в начало или конец. */
__hot int inner_first(MDBX_cursor *mc, MDBX_val *data) { return cursor_brim(true, true, mc, data, nullptr); }

__hot int inner_last(MDBX_cursor *mc, MDBX_val *data) { return cursor_brim(true, false, mc, data, nullptr); }

__hot int outer_first(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data) {
  return cursor_brim(false, true, mc, key, data);
}

__hot int outer_last(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data) {
  return cursor_brim(false, false, mc, key, data);
}

/*----------------------------------------------------------------------------*/

/* Функция-шаблон: Передвигает курсор на одну позицию.
 * При необходимости управляет вложенным курсором. */
__hot int inner_next(MDBX_cursor *mc, MDBX_val *data) { return cursor_step(true, true, mc, data, nullptr, MDBX_NEXT); }

__hot int inner_prev(MDBX_cursor *mc, MDBX_val *data) { return cursor_step(true, false, mc, data, nullptr, MDBX_PREV); }

__hot int outer_next(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data, MDBX_cursor_op op) {
  return cursor_step(false, true, mc, key, data, op);
}

__hot int outer_prev(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data, MDBX_cursor_op op) {
  return cursor_step(false, false, mc, key, data, op);
}
int cursor_on_first(const MDBX_cursor *mc) {
  for (intptr_t i = 0; i <= mc->top; ++i) {
    if (mc->ki[i])
      return MDBX_RESULT_FALSE;
  }
  return MDBX_RESULT_TRUE;
}

int cursor_on_last(const MDBX_cursor *mc) {
  for (intptr_t i = 0; i <= mc->top; ++i) {
    size_t nkeys = page_numkeys(mc->pg[i]);
    if (mc->ki[i] < nkeys - 1)
      return MDBX_RESULT_FALSE;
  }
  return MDBX_RESULT_TRUE;
}
MDBX_INTERNAL cdr_t cursor_forward_at(MDBX_cursor *iter, size_t amount, int level) {
  if (level < iter->top)
    iter->top_and_stash = (uint16_t)level;

  cdr_t tdr = {.err = MDBX_SUCCESS, .distance = amount};
  size_t step = page_numkeys(iter->pg[iter->top]) - iter->ki[iter->top];
  while (tdr.distance >= step) {
    tdr.distance -= step;
    tdr.err = cursor_sibling_right(iter);
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;
    step = page_numkeys(iter->pg[iter->top]);
  }

  ASSERT(tdr.distance < step);
  iter->ki[iter->top] += (indx_t)tdr.distance;
  tdr.distance = 0;
  if (iter->top + 1 < iter->tree->height) {
    tdr.err = tree_deepen_edge(iter, Z_FIRST);
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;
  }
  if (iter->subcur) {
    inner_gone_unconditional(iter);
    const node_t *node = page_node(iter->pg[iter->top], iter->ki[iter->top]);
    if ((node_flags(node) & N_DUP)) {
      tdr.err = cursor_dupsort_setup(iter, node, iter->pg[iter->top]);
      if (unlikely(tdr.err != MDBX_SUCCESS))
        return tdr;
      tdr.err = inner_first(&iter->subcur->cursor, nullptr);
      if (unlikely(tdr.err != MDBX_SUCCESS))
        return tdr;
    }
  }

  be_filled(iter);
  return tdr;
}

MDBX_INTERNAL cdr_t cursor_backward_at(MDBX_cursor *iter, size_t amount, int level) {
  if (level < iter->top)
    iter->top_and_stash = (uint16_t)level;

  cdr_t tdr = {.err = MDBX_SUCCESS, .distance = amount};
  while (tdr.distance > iter->ki[iter->top]) {
    tdr.distance -= iter->ki[iter->top] + 1;
    tdr.err = cursor_sibling_left(iter);
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;
  }

  ASSERT(tdr.distance <= iter->ki[iter->top]);
  iter->ki[iter->top] -= (indx_t)tdr.distance;
  tdr.distance = 0;
  if (iter->top + 1 < iter->tree->height) {
    tdr.err = tree_deepen_edge(iter, Z_LAST);
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;
  }
  if (iter->subcur) {
    inner_gone_unconditional(iter);
    const node_t *node = page_node(iter->pg[iter->top], iter->ki[iter->top]);
    if ((node_flags(node) & N_DUP)) {
      tdr.err = cursor_dupsort_setup(iter, node, iter->pg[iter->top]);
      if (unlikely(tdr.err != MDBX_SUCCESS))
        return tdr;
      tdr.err = inner_last(&iter->subcur->cursor, nullptr);
      if (unlikely(tdr.err != MDBX_SUCCESS))
        return tdr;
    }
  }

  be_filled(iter);
  return tdr;
}

int cursor_scroll_forward(MDBX_cursor *mc, intptr_t amount, int level) {
  ASSERT(amount > 0);
  if (level < mc->tree->height || !mc->subcur)
    return cursor_forward_at(mc, amount, level).err;

  /* запрошенный уровень предполагает учет multivalues/дубликатов */
  int err = MDBX_SUCCESS;
  if (inner_pointed(mc)) {
    cdr_t tdr = cursor_forward_at(&mc->subcur->cursor, amount, level - mc->tree->height);
    err = tdr.err;
    amount = tdr.distance + 1;
    if (unlikely(err != MDBX_NOTFOUND))
      return err;
    inner_gone_unconditional(mc);
  }

  while (true) {
    if (++mc->ki[mc->top] >= page_numkeys(mc->pg[mc->top])) {
      err = cursor_sibling_right(mc);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }

    const node_t *node = page_node(mc->pg[mc->top], mc->ki[mc->top]);
    if ((node->flags & N_DUP) == 0) {
      if (unlikely(--amount == 0))
        return tree_deepen_edge(mc, Z_FIRST);
      continue;
    }

    err = cursor_dupsort_setup(mc, node, mc->pg[mc->top]);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if ((size_t)amount <= mc->subcur->nested_tree.items) {
      err = inner_first(&mc->subcur->cursor, nullptr);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      return cursor_forward_at(&mc->subcur->cursor, (size_t)amount - 1, level - mc->tree->height).err;
    }
    amount -= (size_t)mc->subcur->nested_tree.items;
    inner_gone_unconditional(mc);
  }
}

int cursor_scroll_backward(MDBX_cursor *mc, intptr_t amount, int level) {
  ASSERT(amount > 0);
  if (level < mc->tree->height || !mc->subcur)
    return cursor_backward_at(mc, amount, level).err;

  /* запрошенный уровень предполагает учет multivalues/дубликатов */
  int err = MDBX_SUCCESS;
  if (inner_pointed(mc)) {
    cdr_t tdr = cursor_backward_at(&mc->subcur->cursor, amount, level - mc->tree->height);
    err = tdr.err;
    amount = tdr.distance + 1;
    if (unlikely(err != MDBX_NOTFOUND))
      return err;
    inner_gone_unconditional(mc);
  }

  while (true) {
    intptr_t ki = mc->ki[mc->top] - 1;
    /* mc->ki[mc->top] will be overwritten by cursor_sibling_left() in case ki < 0 here */
    mc->ki[mc->top] = (indx_t)ki;
    if (unlikely(ki < 0)) {
      err = cursor_sibling_left(mc);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }

    const node_t *node = page_node(mc->pg[mc->top], mc->ki[mc->top]);
    if ((node->flags & N_DUP) == 0) {
      if (unlikely(--amount == 0))
        return tree_deepen_edge(mc, Z_LAST);
      continue;
    }

    err = cursor_dupsort_setup(mc, node, mc->pg[mc->top]);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    if ((size_t)amount <= mc->subcur->nested_tree.items) {
      err = inner_last(&mc->subcur->cursor, nullptr);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      return cursor_backward_at(&mc->subcur->cursor, (size_t)amount - 1, level - mc->tree->height).err;
    }
    amount -= (size_t)mc->subcur->nested_tree.items;
    inner_gone_unconditional(mc);
  }
}
