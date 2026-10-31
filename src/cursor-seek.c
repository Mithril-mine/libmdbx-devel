/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file cursor-seek.c — cursor positioning by key (cursor_seek), the cursor
/// operations dispatcher (cursor_ops) and the estimate/distribution
/// machinery (cursor_distance, cursor_distribute); part of the cursor.c
/// module (see also cursor-setup.c / cursor-nav.c / cursor-mut.c and
/// cursor-internal.h).

#include "internals.h"
#include "cursor-internal.h"

__hot csr_t cursor_seek(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data, MDBX_cursor_op op) {
  DKBUF_DEBUG;

  alignkey_t aligned;
  csr_t ret = {.exact = false, .err = check_key(mc, key, &aligned)};
  if (unlikely(ret.err != MDBX_SUCCESS))
    return ret;

  page_t *mp;
  node_t *node = nullptr;
  /* See if we're already on the right page */
  if (is_pointed(mc)) {
    mp = mc->pg[mc->top];
    cASSERT0(mc, is_leaf(mp));
    const size_t nkeys = page_numkeys(mp);
    if (unlikely(nkeys == 0)) {
      /* при создании первой листовой страницы */
      cASSERT0(mc, mc->top == 0 && mc->tree->height == 1 && mc->tree->branch_pages == 0 && mc->tree->leaf_pages == 1 &&
                       mc->ki[0] == 0);
      /* Логически верно, но нет смысла менять флаги, ибо это мимолетная/временная
       * ситуация до добавления элемента выше по стеку вызовов:
         mc->flags |= z_eof_soft | z_hollow; */
      ret.err = MDBX_NOTFOUND;
      return ret;
    }

    MDBX_val nodekey;
    if (is_dupfix_leaf(mp))
      nodekey = page_dupfix_key(mp, 0, mc->tree->dupfix_size);
    else {
      node = page_node(mp, 0);
      nodekey = get_key(node);
      inner_gone(mc);
    }
    int cmp = mc->clc->k.cmp(&aligned.key, &nodekey);
    if (cmp >= 0) {
      if (unlikely(cmp == 0)) {
        /* Probably happens rarely, but first node on the page was the one we wanted. */
        mc->ki[mc->top] = 0;
        ret.exact = true;
        goto got_node;
      }
      /* Искомый ключ больше первого на этой странице,
       * целевая позиция на этой странице, либо на последующих (ближе к концу). */
      if (likely(nkeys > 1)) {
        if (is_dupfix_leaf(mp)) {
          nodekey.iov_base = page_dupfix_ptr(mp, nkeys - 1, nodekey.iov_len);
        } else {
          node = page_node(mp, nkeys - 1);
          nodekey = get_key(node);
        }
        cmp = mc->clc->k.cmp(&aligned.key, &nodekey);
        if (cmp <= 0) {
          if (unlikely(cmp == 0)) {
            /* last node was the one we wanted */
            mc->ki[mc->top] = (indx_t)(nkeys - 1);
            ret.exact = true;
            goto got_node;
          }
          /* Искомый ключ между первым и последним на этой страницы,
           * поэтому пропускаем поиск по дереву и продолжаем только на текущей
           * странице. */
          /* Сравниваем с текущей позицией, ибо частным сценарием является такое
           * совпадение, но не делаем проверку если текущая позиция является
           * первой/последней и соответственно такое сравнение было выше. */
          if (mc->ki[mc->top] > 0 && mc->ki[mc->top] < nkeys - 1) {
            if (is_dupfix_leaf(mp)) {
              nodekey.iov_base = page_dupfix_ptr(mp, mc->ki[mc->top], nodekey.iov_len);
            } else {
              node = page_node(mp, mc->ki[mc->top]);
              nodekey = get_key(node);
            }
            cmp = mc->clc->k.cmp(&aligned.key, &nodekey);
            if (unlikely(cmp == 0)) {
              /* current node was the one we wanted */
              ret.exact = true;
              goto got_node;
            }
          }
          goto search_node;
        }
      }

      /* Если в стеке курсора есть страницы справа, то продолжим искать там. */
      cASSERT0(mc, mc->tree->height > mc->top);
      for (intptr_t i = 0; i < mc->top; i++)
        if ((size_t)mc->ki[i] + 1 < page_numkeys(mc->pg[i]))
          goto continue_other_pages;

      /* Ключ больше последнего. */
      mc->ki[mc->top] = (indx_t)nkeys;
      if (op < MDBX_SET_RANGE) {
      target_not_found:
        cASSERT0(mc, op == MDBX_SET || op == MDBX_SET_KEY || op == MDBX_GET_BOTH || op == MDBX_GET_BOTH_RANGE);
        /* Операция предполагает поиск конкретного ключа, который не найден.
         * Поэтому переводим курсор в неустановленное состояние, но без сброса
         * top, что позволяет работать fastpath при последующем поиске по дереву
         * страниц. */
        mc->flags |= z_hollow;
        if (mc->subcur)
          mc->subcur->cursor.flags |= z_hollow;
        ret.err = MDBX_NOTFOUND;
        return ret;
      }
      cASSERT0(mc, op == MDBX_SET_RANGE);
      mc->flags = z_eof_soft | z_eof_hard | (mc->flags & z_clear_mask);
      ret.err = MDBX_NOTFOUND;
      return ret;
    }

    if (mc->top == 0) {
      /* There are no other pages */
      mc->ki[mc->top] = 0;
      if (op >= MDBX_SET_RANGE)
        goto got_node;
      else
        goto target_not_found;
    }
  }
  cASSERT0(mc, !inner_pointed(mc));

continue_other_pages:
  ret.err = tree_search(mc, &aligned.key, 0);
  if (unlikely(ret.err != MDBX_SUCCESS))
    return ret;

  cASSERT0(mc, is_pointed(mc) && !inner_pointed(mc));
  mp = mc->pg[mc->top];
  MDBX_ANALYSIS_ASSUME(mp != nullptr);
  cASSERT0(mc, is_leaf(mp));

search_node:
  cASSERT0(mc, is_pointed(mc) && !inner_pointed(mc));
  sfr_t sr = tree_search_foliage(mc, &aligned.key);
  node = sr.node;
  ret.exact = sr.exact;
  if (!ret.exact) {
    if (op < MDBX_SET_RANGE)
      goto target_not_found;

    if (node == nullptr) {
      DEBUG("%s", "===> inexact leaf not found, goto sibling");
      ret.err = cursor_sibling_right(mc);
      if (unlikely(ret.err != MDBX_SUCCESS))
        return ret; /* no entries matched */
      mp = mc->pg[mc->top];
      cASSERT0(mc, is_leaf(mp));
      if (!is_dupfix_leaf(mp))
        node = page_node(mp, 0);
    }
  }

got_node:
  cASSERT0(mc, is_pointed(mc) && !inner_pointed(mc));
  cASSERT0(mc, mc->ki[mc->top] < page_numkeys(mc->pg[mc->top]));
  if (!MDBX_DISABLE_VALIDATION && unlikely(!check_leaf_type(mc, mp))) {
    ERROR("unexpected leaf-page #%" PRIaPGNO " type 0x%x seen by cursor", mp->pgno, mp->flags);
    ret.err = MDBX_CORRUPTED;
    return ret;
  }

  if (is_dupfix_leaf(mp)) {
    if (op >= MDBX_SET_KEY)
      *key = page_dupfix_key(mp, mc->ki[mc->top], mc->tree->dupfix_size);
    be_filled(mc);
    ret.err = MDBX_SUCCESS;
    return ret;
  }

  if (node_flags(node) & N_DUP) {
    ret.err = cursor_dupsort_setup(mc, node, mp);
    if (unlikely(ret.err != MDBX_SUCCESS)) {
      /* The sub-page could not be materialized (corruption): the positioned
       * match is not a usable exact match, keep the exact/exact-implies-success
       * invariant. (issue #176) */
      ret.exact = false;
      return ret;
    }
    if (op >= MDBX_SET) {
      MDBX_ANALYSIS_ASSUME(mc->subcur != nullptr);
      if (node_flags(node) & N_TREE) {
        ret.err = inner_first(&mc->subcur->cursor, data);
        if (unlikely(ret.err != MDBX_SUCCESS))
          return ret;
      } else if (data) {
        const page_t *inner_mp = mc->subcur->cursor.pg[0];
        cASSERT0(mc, is_subpage(inner_mp) && is_leaf(inner_mp));
        const size_t inner_ki = mc->subcur->cursor.ki[0];
        if (is_dupfix_leaf(inner_mp))
          *data = page_dupfix_key(inner_mp, inner_ki, mc->tree->dupfix_size);
        else
          *data = get_key(page_node(inner_mp, inner_ki));
      }
    } else {
      MDBX_ANALYSIS_ASSUME(mc->subcur != nullptr);
      ret = cursor_seek(&mc->subcur->cursor, data, nullptr, MDBX_SET_RANGE);
      if (unlikely(ret.err != MDBX_SUCCESS)) {
        if (ret.err == MDBX_NOTFOUND && op < MDBX_SET_RANGE)
          goto target_not_found;
        return ret;
      }
      if (op == MDBX_GET_BOTH && !ret.exact)
        goto target_not_found;
    }
  } else if (likely(data)) {
    if (op <= MDBX_GET_BOTH_RANGE) {
      if (unlikely(data->iov_len < mc->clc->v.lmin || data->iov_len > mc->clc->v.lmax)) {
        ret.err = MDBX_BAD_VALSIZE;
        return ret;
      }
      MDBX_val aligned_data = *data;
      uint64_t aligned_databytes;
      if (mc->tree->flags & MDBX_INTEGERDUP) {
        if (aligned_data.iov_len == 8) {
          if (unlikely(7 & (uintptr_t)aligned_data.iov_base))
            /* copy instead of return error to avoid break compatibility */
            aligned_data.iov_base = bcopy_8(&aligned_databytes, aligned_data.iov_base);
        } else if (aligned_data.iov_len == 4) {
          if (unlikely(3 & (uintptr_t)aligned_data.iov_base))
            /* copy instead of return error to avoid break compatibility */
            aligned_data.iov_base = bcopy_4(&aligned_databytes, aligned_data.iov_base);
        } else {
          ret.err = MDBX_BAD_VALSIZE;
          return ret;
        }
      }
      MDBX_val actual_data;
      ret.err = node_read(mc, node, &actual_data, mc->pg[mc->top]);
      if (unlikely(ret.err != MDBX_SUCCESS))
        return ret;
      const int cmp = mc->clc->v.cmp(&aligned_data, &actual_data);
      if (cmp) {
        if (op != MDBX_GET_BOTH_RANGE) {
          cASSERT0(mc, op == MDBX_GET_BOTH);
          goto target_not_found;
        }
        if (cmp > 0) {
          ret.err = MDBX_NOTFOUND;
          return ret;
        }
      }
      *data = actual_data;
    } else {
      ret.err = node_read(mc, node, data, mc->pg[mc->top]);
      if (unlikely(ret.err != MDBX_SUCCESS))
        return ret;
    }
  }

  /* The key already matches in all other cases */
  if (op >= MDBX_SET_KEY)
    get_key_optional(node, key);

  DEBUG("==> cursor placed on key [%s], data [%s]", DKEY_DEBUG(key), DVAL_DEBUG(data));
  ret.err = MDBX_SUCCESS;
  be_filled(mc);
  return ret;
}

__hot int cursor_ops(MDBX_cursor *mc, MDBX_val *key, MDBX_val *data, const MDBX_cursor_op op) {
  if (op != MDBX_GET_CURRENT)
    DEBUG(">> cursor %p(0x%x), ops %u, key %p, value %p", __Wpedantic_format_voidptr(mc), mc->flags, op,
          __Wpedantic_format_voidptr(key), __Wpedantic_format_voidptr(data));
  int rc;

  cASSERT0(mc, mc->stash < 1);
  switch (op) {
  case MDBX_GET_CURRENT:
    cASSERT0(mc, (mc->flags & z_inner) == 0);
    if (unlikely(!is_filled(mc))) {
      if (is_hollow(mc))
        return MDBX_ENODATA;
      if (mc->ki[mc->top] >= page_numkeys(mc->pg[mc->top]))
        return MDBX_NOTFOUND;
    }
    if (mc->flags & z_after_delete)
      return outer_next(mc, key, data, MDBX_NEXT_NODUP);
    else if (inner_pointed(mc) && (mc->subcur->cursor.flags & z_after_delete))
      return outer_next(mc, key, data, MDBX_NEXT_DUP);
    else {
      const page_t *mp = mc->pg[mc->top];
      const node_t *node = page_node(mp, mc->ki[mc->top]);
      get_key_optional(node, key);
      if (!data)
        return MDBX_SUCCESS;
      if (node_flags(node) & N_DUP) {
        if (!MDBX_DISABLE_VALIDATION && unlikely(!mc->subcur))
          return unexpected_dupsort(mc);
        mc = &mc->subcur->cursor;
        if (unlikely(!is_filled(mc))) {
          if (is_hollow(mc))
            return MDBX_ENODATA;
          if (mc->ki[mc->top] >= page_numkeys(mc->pg[mc->top]))
            return MDBX_NOTFOUND;
        }
        mp = mc->pg[mc->top];
        if (is_dupfix_leaf(mp))
          *data = page_dupfix_key(mp, mc->ki[mc->top], mc->tree->dupfix_size);
        else
          *data = get_key(page_node(mp, mc->ki[mc->top]));
        return MDBX_SUCCESS;
      } else {
        cASSERT0(mc, !inner_pointed(mc));
        return node_read(mc, node, data, mc->pg[mc->top]);
      }
    }

  case MDBX_GET_BOTH:
  case MDBX_GET_BOTH_RANGE:
    if (unlikely(data == nullptr))
      return MDBX_EINVAL;
    if (unlikely(mc->subcur == nullptr))
      return MDBX_INCOMPATIBLE;
    /* fall through */
    __fallthrough;
  case MDBX_SET:
  case MDBX_SET_KEY:
  case MDBX_SET_RANGE:
    if (unlikely(key == nullptr))
      return MDBX_EINVAL;
    rc = cursor_seek(mc, key, data, op).err;
    if (rc == MDBX_SUCCESS)
      cASSERT0(mc, is_filled(mc));
    else if (rc == MDBX_NOTFOUND && mc->tree->items) {
      cASSERT0(mc, is_pointed(mc));
      cASSERT0(mc, op == MDBX_SET_RANGE || op == MDBX_GET_BOTH_RANGE || is_hollow(mc));
      cASSERT0(mc, op == MDBX_GET_BOTH_RANGE || inner_hollow(mc));
    } else
      cASSERT0(mc, is_poor(mc) && !is_filled(mc));
    return rc;

  case MDBX_SEEK_AND_GET_MULTIPLE:
    MPROBE_WATCH(cursor_multiple_seek, 1);
    if (unlikely(!key))
      return MDBX_EINVAL;
    rc = cursor_seek(mc, key, data, MDBX_SET).err;
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    __fallthrough /* fall through */;
  case MDBX_GET_MULTIPLE:
    MPROBE_WATCH(cursor_multiple_get, 1);
    if (unlikely(!data))
      return MDBX_EINVAL;
    if (unlikely((mc->tree->flags & MDBX_DUPFIXED) == 0))
      return MDBX_INCOMPATIBLE;
    if (unlikely(!is_filled(mc)))
      return MDBX_ENODATA;
    if (key) {
      const page_t *mp = mc->pg[mc->top];
      const node_t *node = page_node(mp, mc->ki[mc->top]);
      *key = get_key(node);
    }
    cASSERT0(mc, is_filled(mc));
    if (unlikely(!inner_filled(mc))) {
      if (inner_pointed(mc))
        return MDBX_ENODATA;
      const page_t *mp = mc->pg[mc->top];
      const node_t *node = page_node(mp, mc->ki[mc->top]);
      return node_read(mc, node, data, mp);
    }
    goto fetch_multiple;

  case MDBX_NEXT_MULTIPLE:
    MPROBE_WATCH(cursor_multiple_next, 1);
    if (unlikely(!data))
      return MDBX_EINVAL;
    if (unlikely(mc->subcur == nullptr))
      return MDBX_INCOMPATIBLE;
    rc = outer_next(mc, key, data, MDBX_NEXT_DUP);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    else {
    fetch_multiple:
      cASSERT0(mc, is_filled(mc) && inner_filled(mc));
      MDBX_cursor *mx = &mc->subcur->cursor;
      data->iov_len = page_numkeys(mx->pg[mx->top]) * mx->tree->dupfix_size;
      data->iov_base = page2payload(mx->pg[mx->top]);
      mx->ki[mx->top] = (indx_t)page_numkeys(mx->pg[mx->top]) - 1;
      return MDBX_SUCCESS;
    }

  case MDBX_PREV_MULTIPLE:
    MPROBE_WATCH(cursor_multiple_prev, 1);
    if (unlikely(!data))
      return MDBX_EINVAL;
    if (unlikely(mc->subcur == nullptr))
      return MDBX_INCOMPATIBLE;
    if (unlikely(!is_filled(mc) || !inner_filled(mc)))
      return MDBX_ENODATA;
    rc = cursor_sibling_left(&mc->subcur->cursor);
    if (likely(rc == MDBX_SUCCESS))
      goto fetch_multiple;
    return rc;

  case MDBX_NEXT_DUP:
  case MDBX_NEXT:
  case MDBX_NEXT_NODUP:
    rc = outer_next(mc, key, data, op);
    /* The z_eof_hard flag reflects internal intermediate state and must not be exposed to outer code after an next-ops.
     * Elsewise a tree rebalance after deletion may going wrong. So just clear z_eof_hard flag in both cursors anyway.
     */
    mc->flags &= ~z_eof_hard;
    ((cursor_couple_t *)mc)->inner.cursor.flags &= ~z_eof_hard;
    return rc;

  case MDBX_PREV_DUP:
  case MDBX_PREV:
  case MDBX_PREV_NODUP:
    return outer_prev(mc, key, data, op);

  case MDBX_FIRST:
    return outer_first(mc, key, data);
  case MDBX_LAST:
    return outer_last(mc, key, data);

  case MDBX_LAST_DUP:
  case MDBX_FIRST_DUP:
    if (unlikely(data == nullptr))
      return MDBX_EINVAL;
    if (unlikely(!is_filled(mc)))
      return MDBX_ENODATA;
    else {
      node_t *node = page_node(mc->pg[mc->top], mc->ki[mc->top]);
      get_key_optional(node, key);
      if ((node_flags(node) & N_DUP) == 0)
        return node_read(mc, node, data, mc->pg[mc->top]);
      else if (MDBX_DISABLE_VALIDATION || likely(mc->subcur))
        return ((op == MDBX_FIRST_DUP) ? inner_first : inner_last)(&mc->subcur->cursor, data);
      else
        return unexpected_dupsort(mc);
    }
    __unreachable();

  case MDBX_SET_UPPERBOUND:
  case MDBX_SET_LOWERBOUND:
    if (unlikely(key == nullptr || data == nullptr))
      return MDBX_EINVAL;
    else {
      MDBX_val save_data = *data;
      csr_t csr = cursor_seek(mc, key, data, MDBX_SET_RANGE);
      rc = csr.err;
      if (rc == MDBX_SUCCESS && csr.exact && mc->subcur) {
        csr.exact = false;
        if (!save_data.iov_base) {
          /* Avoiding search nested dupfix hive if no data provided.
           * This is changes the semantic of MDBX_SET_LOWERBOUND but avoid
           * returning MDBX_BAD_VALSIZE. */
        } else if (is_pointed(&mc->subcur->cursor)) {
          *data = save_data;
          csr = cursor_seek(&mc->subcur->cursor, data, nullptr, MDBX_SET_RANGE);
          rc = csr.err;
          if (rc == MDBX_NOTFOUND) {
            cASSERT0(mc, !csr.exact);
            rc = outer_next(mc, key, data, MDBX_NEXT_NODUP);
          }
        } else {
          int cmp = mc->clc->v.cmp(&save_data, data);
          csr.exact = (cmp == 0);
          if (cmp > 0)
            rc = outer_next(mc, key, data, MDBX_NEXT_NODUP);
        }
      }
      if (rc == MDBX_SUCCESS && !csr.exact)
        rc = MDBX_RESULT_TRUE;
      if (unlikely(op == MDBX_SET_UPPERBOUND)) {
        /* minor fixups for MDBX_SET_UPPERBOUND */
        if (rc == MDBX_RESULT_TRUE)
          /* already at great-than by MDBX_SET_LOWERBOUND */
          rc = MDBX_SUCCESS;
        else if (rc == MDBX_SUCCESS)
          /* exactly match, going next */
          rc = outer_next(mc, key, data, MDBX_NEXT);
      }
    }
    return rc;

  /* Doubtless API to positioning of the cursor at a specified key. */
  case MDBX_TO_KEY_LESSER_THAN:
  case MDBX_TO_KEY_LESSER_OR_EQUAL:
  case MDBX_TO_KEY_EQUAL:
  case MDBX_TO_KEY_GREATER_OR_EQUAL:
  case MDBX_TO_KEY_GREATER_THAN:
    if (unlikely(key == nullptr))
      return MDBX_EINVAL;
    else {
      csr_t csr = cursor_seek(mc, key, data, MDBX_SET_RANGE);
      rc = csr.err;
      if (csr.exact) {
        cASSERT0(mc, csr.err == MDBX_SUCCESS);
        if (op == MDBX_TO_KEY_LESSER_THAN)
          rc = outer_prev(mc, key, data, MDBX_PREV_NODUP);
        else if (op == MDBX_TO_KEY_GREATER_THAN)
          rc = outer_next(mc, key, data, MDBX_NEXT_NODUP);
      } else if (op < MDBX_TO_KEY_EQUAL && (rc == MDBX_NOTFOUND || rc == MDBX_SUCCESS))
        rc = outer_prev(mc, key, data, MDBX_PREV_NODUP);
      else if (op == MDBX_TO_KEY_EQUAL && rc == MDBX_SUCCESS)
        rc = MDBX_NOTFOUND;
    }
    return rc;

  /* Doubtless API to positioning of the cursor at a specified key-value pair
   * for multi-value hives. */
  case MDBX_TO_EXACT_KEY_VALUE_LESSER_THAN:
  case MDBX_TO_EXACT_KEY_VALUE_LESSER_OR_EQUAL:
  case MDBX_TO_EXACT_KEY_VALUE_EQUAL:
  case MDBX_TO_EXACT_KEY_VALUE_GREATER_OR_EQUAL:
  case MDBX_TO_EXACT_KEY_VALUE_GREATER_THAN:
    if (unlikely(key == nullptr || data == nullptr))
      return MDBX_EINVAL;
    else {
      MDBX_val save_data = *data;
      csr_t csr = cursor_seek(mc, key, data, MDBX_SET_KEY);
      rc = csr.err;
      if (rc == MDBX_SUCCESS) {
        cASSERT0(mc, csr.exact);
        if (inner_pointed(mc)) {
          MDBX_cursor *const mx = &mc->subcur->cursor;
          csr = cursor_seek(mx, &save_data, nullptr, MDBX_SET_RANGE);
          rc = csr.err;
          if (csr.exact) {
            cASSERT0(mc, csr.err == MDBX_SUCCESS);
            if (op == MDBX_TO_EXACT_KEY_VALUE_LESSER_THAN)
              rc = inner_prev(mx, data);
            else if (op == MDBX_TO_EXACT_KEY_VALUE_GREATER_THAN)
              rc = inner_next(mx, data);
          } else if (op < MDBX_TO_EXACT_KEY_VALUE_EQUAL && (rc == MDBX_NOTFOUND || rc == MDBX_SUCCESS))
            rc = inner_prev(mx, data);
          else if (op == MDBX_TO_EXACT_KEY_VALUE_EQUAL && rc == MDBX_SUCCESS)
            rc = MDBX_NOTFOUND;
        } else {
          int cmp = mc->clc->v.cmp(data, &save_data);
          switch (op) {
          default:
            __unreachable();
          case MDBX_TO_EXACT_KEY_VALUE_LESSER_THAN:
            rc = (cmp < 0) ? MDBX_SUCCESS : MDBX_NOTFOUND;
            break;
          case MDBX_TO_EXACT_KEY_VALUE_LESSER_OR_EQUAL:
            rc = (cmp <= 0) ? MDBX_SUCCESS : MDBX_NOTFOUND;
            break;
          case MDBX_TO_EXACT_KEY_VALUE_EQUAL:
            rc = (cmp == 0) ? MDBX_SUCCESS : MDBX_NOTFOUND;
            break;
          case MDBX_TO_EXACT_KEY_VALUE_GREATER_OR_EQUAL:
            rc = (cmp >= 0) ? MDBX_SUCCESS : MDBX_NOTFOUND;
            break;
          case MDBX_TO_EXACT_KEY_VALUE_GREATER_THAN:
            rc = (cmp > 0) ? MDBX_SUCCESS : MDBX_NOTFOUND;
            break;
          }
        }
      }
    }
    return rc;

  case MDBX_TO_PAIR_LESSER_THAN:
  case MDBX_TO_PAIR_LESSER_OR_EQUAL:
  case MDBX_TO_PAIR_EQUAL:
  case MDBX_TO_PAIR_GREATER_OR_EQUAL:
  case MDBX_TO_PAIR_GREATER_THAN:
    if (unlikely(key == nullptr || data == nullptr))
      return MDBX_EINVAL;
    else {
      MDBX_val save_data = *data;
      csr_t csr = cursor_seek(mc, key, data, MDBX_SET_RANGE);
      rc = csr.err;
      if (csr.exact) {
        cASSERT0(mc, csr.err == MDBX_SUCCESS);
        if (inner_pointed(mc)) {
          MDBX_cursor *const mx = &mc->subcur->cursor;
          csr = cursor_seek(mx, &save_data, nullptr, MDBX_SET_RANGE);
          rc = csr.err;
          if (csr.exact) {
            cASSERT0(mc, csr.err == MDBX_SUCCESS);
            if (op == MDBX_TO_PAIR_LESSER_THAN)
              rc = outer_prev(mc, key, data, MDBX_PREV);
            else if (op == MDBX_TO_PAIR_GREATER_THAN)
              rc = outer_next(mc, key, data, MDBX_NEXT);
          } else if (op < MDBX_TO_PAIR_EQUAL && (rc == MDBX_NOTFOUND || rc == MDBX_SUCCESS))
            rc = outer_prev(mc, key, data, MDBX_PREV);
          else if (op == MDBX_TO_PAIR_EQUAL && rc == MDBX_SUCCESS)
            rc = MDBX_NOTFOUND;
          else if (op > MDBX_TO_PAIR_EQUAL && rc == MDBX_NOTFOUND)
            rc = outer_next(mc, key, data, MDBX_NEXT);
        } else {
          int cmp = mc->clc->v.cmp(data, &save_data);
          switch (op) {
          default:
            __unreachable();
          case MDBX_TO_PAIR_LESSER_THAN:
            if (cmp >= 0)
              rc = outer_prev(mc, key, data, MDBX_PREV);
            break;
          case MDBX_TO_PAIR_LESSER_OR_EQUAL:
            if (cmp > 0)
              rc = outer_prev(mc, key, data, MDBX_PREV);
            break;
          case MDBX_TO_PAIR_EQUAL:
            rc = (cmp == 0) ? MDBX_SUCCESS : MDBX_NOTFOUND;
            break;
          case MDBX_TO_PAIR_GREATER_OR_EQUAL:
            if (cmp < 0)
              rc = outer_next(mc, key, data, MDBX_NEXT);
            break;
          case MDBX_TO_PAIR_GREATER_THAN:
            if (cmp <= 0)
              rc = outer_next(mc, key, data, MDBX_NEXT);
            break;
          }
        }
      } else if (op < MDBX_TO_PAIR_EQUAL && (rc == MDBX_NOTFOUND || rc == MDBX_SUCCESS))
        rc = outer_prev(mc, key, data, MDBX_PREV_NODUP);
      else if (op == MDBX_TO_PAIR_EQUAL && rc == MDBX_SUCCESS)
        rc = MDBX_NOTFOUND;
    }
    return rc;

  default:
    DEBUG("unhandled/unimplemented cursor operation %u", op);
    return MDBX_EINVAL;
  }
}
static cdr_t cursor_distance_at(MDBX_cursor *iter, const MDBX_cursor *end, int level) {
  cdr_t tdr = {.err = MDBX_SUCCESS, .distance = 0};
  if (level < iter->top)
    iter->top_and_stash = (uint16_t)level;
  ASSERT(end->top >= iter->top);
  ASSERT(end->tree == iter->tree || memcmp(end->tree, iter->tree, sizeof(tree_t)) == 0);
  const int iter_ki = iter->ki[iter->top] + ((iter->flags & z_eof_hard) != 0 && level + 1 == iter->tree->height);
  const int end_ki = end->ki[iter->top] + ((end->flags & z_eof_hard) != 0 && level + 1 == iter->tree->height);
  if (iter->pg[iter->top] == end->pg[iter->top])
    tdr.distance = end_ki - iter_ki;
  else {
    tdr.distance = page_numkeys(iter->pg[iter->top]) - iter_ki;
    while (true) {
      tdr.err = cursor_sibling_right(iter);
      if (unlikely(tdr.err != MDBX_SUCCESS))
        return tdr;
      if (iter->pg[iter->top] == end->pg[iter->top])
        break;
      tdr.distance += page_numkeys(iter->pg[iter->top]);
    }
    tdr.distance += end_ki;
  }
  return tdr;
}

static cdr_t cursor_leaf_amount(const page_t *mp, intptr_t begin, intptr_t end) {
  cdr_t tdr = {.err = MDBX_SUCCESS, .distance = end - begin};
  intptr_t ki = begin;
  do {
    const node_t *node = page_node(mp, ki);
    const size_t dsize = node_ds(node);
    if (node->flags & N_TREE) {
      if (!MDBX_DISABLE_VALIDATION && unlikely(dsize < sizeof(tree_t))) {
        tdr.err = bad_page(mp, "invalid %s-db record size (%zu, expect %zu)\n", "nested", dsize, sizeof(tree_t));
        return tdr;
      }
      tree_t subtree;
      memcpy(&subtree, node_data(node), sizeof(tree_t));
      tdr.distance += (size_t)subtree.items - 1;
    } else if (node->flags & N_DUP) {
      if (!MDBX_DISABLE_VALIDATION && unlikely(node_ds(node) <= PAGEHDRSZ)) {
        tdr.err = bad_page(mp, "invalid nested/sub-page record size (%zu, expect %u)\n", dsize, PAGEHDRSZ);
        return tdr;
      }
      tdr.distance += page_numkeys(node_data(node)) - 1;
    }
  } while (++ki < end);
  return tdr;
}

cdr_t cursor_distance(MDBX_cursor *iter, const MDBX_cursor *end, int level) {
  ASSERT(iter->tree == end->tree);
  cdr_t tdr = {.err = MDBX_SUCCESS, .distance = 0};
  const intptr_t outer_diff_level = tree_diff_level(iter, end);
  if (unlikely(outer_diff_level < MDBX_RESULT_TRUE)) {
  beyond_end:
    tdr.err = MDBX_ENODATA;
    return tdr;
  }

  if (level < outer_diff_level)
    return tdr;

  if (!iter->subcur || level < iter->tree->height)
    return cursor_distance_at(iter, end, level);

  if (outer_diff_level == MDBX_RESULT_TRUE) {
    const int cmp = (iter->flags & z_eof_hard) - (end->flags & z_eof_hard);
    if (unlikely(cmp > 0))
      goto beyond_end;
    if (cmp == 0 && inner_pointed(iter)) {
      const intptr_t inner_diff_level = tree_diff_level(&iter->subcur->cursor, &end->subcur->cursor);
      if (unlikely(inner_diff_level < MDBX_RESULT_TRUE))
        goto beyond_end;
    }
  }

  /* запрошенный уровень предполагает учет multivalues/дубликатов */
  if (inner_pointed(iter) && iter->subcur->nested_tree.height <= 1) {
    /* The leaf amounts below count all duplicates of iter's node, so discount
     * the ones before iter's position. The rank is measured within the
     * sub-tree, which is page-walked only when it is a single-level sub-page;
     * deeper nested trees are left with the pre-fix accounting until the
     * multi-page nested-tree walking defect (independent of this off-by-one)
     * is resolved. */
    MDBX_cursor first = iter->subcur->cursor;
    tdr.err = inner_first(&first, nullptr);
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;
    const cdr_t rank = cursor_distance_at(&first, &iter->subcur->cursor, level - iter->tree->height);
    tdr.err = rank.err;
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;
    tdr.distance = 0 - rank.distance;
  }

  while (iter->pg[iter->top] != end->pg[iter->top]) {
    cdr_t leaf_tdr = cursor_leaf_amount(iter->pg[iter->top], iter->ki[iter->top], page_numkeys(iter->pg[iter->top]));
    tdr.distance += leaf_tdr.distance;
    tdr.err = leaf_tdr.err;
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;
    tdr.err = cursor_sibling_right(iter);
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;
  }

  if (iter->ki[iter->top] < end->ki[iter->top]) {
    cdr_t leaf_tdr = cursor_leaf_amount(iter->pg[iter->top], iter->ki[iter->top], end->ki[iter->top]);
    tdr.distance += leaf_tdr.distance;
    tdr.err = leaf_tdr.err;
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;
  }

  ASSERT(end->subcur);
  if (is_pointed(&end->subcur->cursor)) {
    iter->pg[iter->top] = end->pg[iter->top];
    iter->ki[iter->top] = end->ki[iter->top];
    tdr.err = cursor_dupsort_setup(iter, page_node(iter->pg[iter->top], iter->ki[iter->top]), iter->pg[iter->top]);
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;
    tdr.err = inner_first(&iter->subcur->cursor, nullptr);
    if (unlikely(tdr.err != MDBX_SUCCESS))
      return tdr;

    cdr_t subtree_tdr = cursor_distance_at(&iter->subcur->cursor, &end->subcur->cursor, level - iter->tree->height);
    tdr.distance += subtree_tdr.distance;
    tdr.err = subtree_tdr.err;
  }

  return tdr;
}

MDBX_MAYBE_UNUSED static intptr_t debug_cursor_distance(const MDBX_cursor *begin, const MDBX_cursor *end,
                                                        unsigned deepness) {
  intptr_t distance = 0;
  return (mdbx_cursor_distance(begin, end, &distance, deepness) == MDBX_SUCCESS) ? distance : INTPTR_MIN;
}

int cursor_distribute(const MDBX_cursor *begin, const MDBX_cursor *end, MDBX_cursor **array, size_t array_size,
                      int level) {
  const intptr_t outer_diff_level = tree_diff_level(begin, end);
  if (unlikely(outer_diff_level < MDBX_RESULT_TRUE))
    return MDBX_ENODATA;

  const intptr_t inner_diff_level = (outer_diff_level > MDBX_RESULT_TRUE || !inner_pointed(begin))
                                        ? MDBX_RESULT_TRUE
                                        : tree_diff_level(&begin->subcur->cursor, &end->subcur->cursor);
  if (outer_diff_level == MDBX_RESULT_TRUE && unlikely(inner_diff_level < MDBX_RESULT_TRUE))
    return MDBX_ENODATA;

  if (unlikely(level < outer_diff_level))
    return MDBX_RESULT_TRUE;
  if (unlikely(outer_diff_level == MDBX_RESULT_TRUE && inner_diff_level == MDBX_RESULT_TRUE)) {
    /* Курсоры begin и end совпадают */
    return MDBX_RESULT_TRUE;
  }

  MDBX_cursor *iter = (MDBX_cursor *)begin;
  for (intptr_t i = array_size; --i >= 0;) {
    if (array[i] != begin && array[i] != end) {
      iter = array[i];
      break;
    }
  }
  cursor_copy_position(begin, iter);

  const cdr_t tdr = cursor_distance(iter, end, level);
  const size_t amount = tdr.distance;
  int rc = tdr.err;
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  ASSERT(tdr.distance > 0);
  cursor_copy_position(begin, iter);
  size_t processed = 0;
  for (size_t i = 0; i < array_size; ++i) {
    MDBX_cursor *const mc = array[i];
    if (unlikely(processed >= amount)) {
      be_poor(mc);
      rc = MDBX_RESULT_TRUE;
    } else {
      const size_t left_cursors = array_size - i;
      const size_t left_amount = amount - processed;
      const size_t chunk = (left_amount + left_cursors - 1) / left_cursors;
      if (mc != iter) {
        /* hand the iteration over to the cursor being filled, so a cursor whose
         * position is already distributed (e.g. iter itself when `begin` follows
         * it in the array) is never moved again */
        cursor_copy_position(iter, mc);
        iter = mc;
      }
      int err = cursor_scroll_forward(iter, chunk, level);
      processed += chunk;
      VERBOSE("cursor %zu of %zu, chunk %zu of %zu, left %zu, distance %zi, expected %zi, err %i", i, array_size, chunk,
              amount, amount - processed, processed, debug_cursor_distance(begin, iter, level), err);
      if (unlikely(err != MDBX_SUCCESS)) {
        if (err != MDBX_NOTFOUND)
          return rc = err;
        be_poor(iter);
        processed = tdr.distance;
        rc = MDBX_RESULT_TRUE;
      } else {
        intptr_t cmp = cursor_cmp(iter, end);
        if (cmp == 0 && inner_pointed(end))
          cmp = cursor_cmp(&iter->subcur->cursor, &end->subcur->cursor);
        if (cmp > 0) {
          be_poor(iter);
          processed = tdr.distance;
          rc = MDBX_RESULT_TRUE;
        }
      }
    }
  }
  return rc;
}
