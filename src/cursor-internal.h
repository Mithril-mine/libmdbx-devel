/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file cursor-internal.h — static __always_inline helper-templates shared by
/// the cursor.c parts (cursor-setup.c / cursor-nav.c / cursor-mut.c /
/// cursor-seek.c). Header-only by design: no linkage in either build mode.
///
/// Must be included AFTER "internals.h" (consumers rely on cursor.h/cogs.h
/// definitions); the file itself deliberately does not include anything.

#pragma once

static __always_inline int sibling(MDBX_cursor *mc, bool right) {
  if (mc->top < 1) {
    /* root has no siblings */
    return MDBX_NOTFOUND;
  }

  cursor_enroot(mc, 1);
  DEBUG("parent page is page %" PRIaPGNO ", index %u", mc->pg[mc->top]->pgno, mc->ki[mc->top]);

  int err;
  if (right ? (mc->ki[mc->top] + (size_t)1 >= page_numkeys(mc->pg[mc->top])) : (mc->ki[mc->top] == 0)) {
    DEBUG("no more keys aside, moving to next %s sibling", right ? "right" : "left");
    err = right ? cursor_sibling_right(mc) : cursor_sibling_left(mc);
    if (unlikely(err != MDBX_SUCCESS)) {
      if (likely(err == MDBX_NOTFOUND)) {
        cursor_undo_enroot(mc, 1);
        return err;
      }
      goto bailout;
    }
  } else {
    mc->ki[mc->top] += right ? 1 : -1;
    DEBUG("just moving to %s index key %u", right ? "right" : "left", mc->ki[mc->top]);
  }
  cASSERT0(mc, is_branch(mc->pg[mc->top]));

  page_t *mp = mc->pg[mc->top];
  const node_t *node = page_node(mp, mc->ki[mc->top]);
  err = page_get(mc, node_pgno(node), &mp, mp->txnid);
  if (likely(err == MDBX_SUCCESS)) {
    err = cursor_push(mc, mp, right ? 0 : (indx_t)page_numkeys(mp) - 1);
    if (likely(err == MDBX_SUCCESS))
      return err;
  }

bailout:
  cASSERT0(mc, err != MDBX_NOTFOUND);
  be_poor(mc);
  return err;
}

/* Функция-шаблон: Приземляет курсор на данные в текущей позиции.
 * В том числе, загружает данные во вложенный курсор при его наличии. */
static __always_inline int cursor_bring(const bool inner, const bool tend2first, MDBX_cursor *__restrict mc,
                                        MDBX_val *__restrict key, MDBX_val *__restrict data, bool eof) {
  if (inner) {
    cASSERT0(mc, !data && !mc->subcur && (mc->flags & z_inner) != 0);
  } else {
    cASSERT0(mc, (mc->flags & z_inner) == 0);
  }

  const page_t *mp = mc->pg[mc->top];
  if (!MDBX_DISABLE_VALIDATION && unlikely(!check_leaf_type(mc, mp))) {
    ERROR("unexpected leaf-page #%" PRIaPGNO " type 0x%x seen by cursor", mp->pgno, mp->flags);
    return MDBX_CORRUPTED;
  }

  const size_t nkeys = page_numkeys(mp);
  cASSERT0(mc, nkeys > 0);
  const size_t ki = mc->ki[mc->top];
  cASSERT0(mc, nkeys > ki);
  cASSERT0(mc, !eof || ki == nkeys - 1);

  if (inner && is_dupfix_leaf(mp)) {
    be_filled(mc);
    if (eof)
      mc->flags |= z_eof_soft;
    if (likely(key))
      *key = page_dupfix_key(mp, ki, mc->tree->dupfix_size);
    return MDBX_SUCCESS;
  }

  const node_t *__restrict node = page_node(mp, ki);
  if (!inner && (node_flags(node) & N_DUP)) {
    int err = cursor_dupsort_setup(mc, node, mp);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
    MDBX_ANALYSIS_ASSUME(mc->subcur != nullptr);
    if (node_flags(node) & N_TREE) {
      err = tend2first ? inner_first(&mc->subcur->cursor, data) : inner_last(&mc->subcur->cursor, data);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    } else {
      if (!tend2first) {
        mc->subcur->cursor.ki[0] = (indx_t)mc->subcur->nested_tree.items - 1;
        mc->subcur->cursor.flags |= z_eof_soft;
      }
      if (data) {
        const page_t *inner_mp = mc->subcur->cursor.pg[0];
        cASSERT0(mc, is_subpage(inner_mp) && is_leaf(inner_mp));
        const size_t inner_ki = mc->subcur->cursor.ki[0];
        if (is_dupfix_leaf(inner_mp))
          *data = page_dupfix_key(inner_mp, inner_ki, mc->tree->dupfix_size);
        else
          *data = get_key(page_node(inner_mp, inner_ki));
      }
    }
    be_filled(mc);
  } else {
    if (!inner)
      inner_gone(mc);
    if (data) {
      int err = node_read(mc, node, data, mp);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
    be_filled(mc);
    if (eof)
      mc->flags |= z_eof_soft;
  }

  get_key_optional(node, key);
  return MDBX_SUCCESS;
}

/* Функция-шаблон: Устанавливает курсор в начало или конец. */
static __always_inline int cursor_brim(const bool inner, const bool tend2first, MDBX_cursor *__restrict mc,
                                       MDBX_val *__restrict key, MDBX_val *__restrict data) {
  if (mc->top != 0) {
    int err = tree_search(mc, nullptr, tend2first ? Z_FIRST : Z_LAST);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }
  const size_t nkeys = page_numkeys(mc->pg[mc->top]);
  cASSERT0(mc, nkeys > 0);
  mc->ki[mc->top] = tend2first ? 0 : (indx_t)nkeys - 1;
  return cursor_bring(inner, tend2first, mc, key, data, !tend2first);
}

/* Функция-шаблон: Передвигает курсор на одну позицию.
 * При необходимости управляет вложенным курсором. */
static __always_inline int cursor_step(const bool inner, const bool forward, MDBX_cursor *__restrict mc,
                                       MDBX_val *__restrict key, MDBX_val *__restrict data, MDBX_cursor_op op) {
  if (forward) {
    if (inner)
      cASSERT0(mc, op == MDBX_NEXT);
    else
      cASSERT0(mc, op == MDBX_NEXT || op == MDBX_NEXT_DUP || op == MDBX_NEXT_NODUP);
  } else {
    if (inner)
      cASSERT0(mc, op == MDBX_PREV);
    else
      cASSERT0(mc, op == MDBX_PREV || op == MDBX_PREV_DUP || op == MDBX_PREV_NODUP);
  }
  if (inner) {
    cASSERT0(mc, !data && !mc->subcur && (mc->flags & z_inner) != 0);
  } else {
    cASSERT0(mc, (mc->flags & z_inner) == 0);
  }

  if (unlikely(is_poor(mc))) {
    int state = mc->flags;
    if (state & z_fresh) {
      if (forward)
        return inner ? inner_first(mc, key) : outer_first(mc, key, data);
      else
        return inner ? inner_last(mc, key) : outer_last(mc, key, data);
    }
    mc->flags = inner ? z_inner | z_poor_mark : z_poor_mark;
    return (state & z_after_delete) ? MDBX_NOTFOUND : MDBX_ENODATA;
  }

  const page_t *mp = mc->pg[mc->top];
  const intptr_t nkeys = page_numkeys(mp);
  cASSERT0(mc, nkeys > 0);

  intptr_t ki = mc->ki[mc->top];
  const uint8_t state = mc->flags & (z_after_delete | z_hollow | z_eof_hard | z_eof_soft);
  if (likely(state == 0)) {
    cASSERT0(mc, ki < nkeys);
    if (!inner && op != (forward ? MDBX_NEXT_NODUP : MDBX_PREV_NODUP)) {
      int err = MDBX_NOTFOUND;
      if (inner_pointed(mc)) {
        err = forward ? inner_next(&mc->subcur->cursor, data) : inner_prev(&mc->subcur->cursor, data);
        if (likely(err == MDBX_SUCCESS)) {
          get_key_optional(page_node(mp, ki), key);
          return MDBX_SUCCESS;
        }
        if (unlikely(err != MDBX_NOTFOUND && err != MDBX_ENODATA)) {
          cASSERT0(mc, !inner_pointed(mc));
          return err;
        }
        cASSERT0(mc, !forward || (mc->subcur->cursor.flags & z_eof_soft));
      }
      if (op == (forward ? MDBX_NEXT_DUP : MDBX_PREV_DUP))
        return err;
    }
  } else {
    if (mc->flags & z_hollow) {
      cASSERT0(mc, !inner_pointed(mc) || inner_hollow(mc));
      return MDBX_ENODATA;
    }

    if (!inner && op == (forward ? MDBX_NEXT_DUP : MDBX_PREV_DUP))
      return MDBX_NOTFOUND;

    if (forward) {
      if (state & z_after_delete) {
        if (ki < nkeys)
          goto bring;
      } else {
        cASSERT0(mc, state & (z_eof_soft | z_eof_hard));
        return MDBX_NOTFOUND;
      }
    } else if (state & z_eof_hard) {
      mc->ki[mc->top] = (indx_t)nkeys - 1;
      goto bring;
    }
  }

  DEBUG("turn-%s: top page was %" PRIaPGNO " in cursor %p, ki %zi of %zi", forward ? "next" : "prev", mp->pgno,
        __Wpedantic_format_voidptr(mc), ki, nkeys);
  if (forward) {
    if (likely(++ki < nkeys))
      mc->ki[mc->top] = (indx_t)ki;
    else {
      DEBUG("%s", "=====> move to next sibling page");
      int err = cursor_sibling_right(mc);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      mp = mc->pg[mc->top];
      DEBUG("next page is %" PRIaPGNO ", key index %u", mp->pgno, mc->ki[mc->top]);
    }
  } else {
    if (likely(--ki >= 0))
      mc->ki[mc->top] = (indx_t)ki;
    else {
      DEBUG("%s", "=====> move to prev sibling page");
      int err = cursor_sibling_left(mc);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      mp = mc->pg[mc->top];
      DEBUG("prev page is %" PRIaPGNO ", key index %u", mp->pgno, mc->ki[mc->top]);
    }
  }
  DEBUG("==> cursor points to page %" PRIaPGNO " with %zu keys, key index %u", mp->pgno, page_numkeys(mp),
        mc->ki[mc->top]);

bring:
  return cursor_bring(inner, forward, mc, key, data, false);
}
