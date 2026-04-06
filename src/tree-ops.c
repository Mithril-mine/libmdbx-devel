/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanations license change,
/// credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

void recalculate_merge_thresholds(MDBX_env *env) {
  const size_t whole_page_space = page_space(env);
  env->merge_threshold = (uint16_t)(whole_page_space - (whole_page_space * env->options.merge_threshold_dot16 >> 16));
  eASSERT0(env, env->merge_threshold >= whole_page_space / 2u && env->merge_threshold <= whole_page_space * 63u / 64u);
}

int tree_drop(MDBX_cursor *mc, const bool may_have_tables) {
  MDBX_txn *txn = mc->txn;
  int rc = tree_search(mc, nullptr, Z_FIRST);
  if (likely(rc == MDBX_SUCCESS)) {
    /* DUPSORT sub-DBs have no large-pages/tables. Omit scanning leaves.
     * This also avoids any P_DUPFIX pages, which have no nodes.
     * Also if the DB doesn't have sub-DBs and has no large/overflow
     * pages, omit scanning leaves. */
    if (!(may_have_tables | mc->tree->large_pages))
      cursor_pop(mc);

    rc = pnl_need(&txn->wr.retired_pages,
                  (size_t)mc->tree->branch_pages + (size_t)mc->tree->leaf_pages + (size_t)mc->tree->large_pages);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;

    page_t *stack[CURSOR_STACK_SIZE];
    for (intptr_t i = 0; i <= mc->top; ++i)
      stack[i] = mc->pg[i];

    while (mc->top >= 0) {
      page_t *const mp = mc->pg[mc->top];
      const size_t nkeys = page_numkeys(mp);
      if (is_leaf(mp)) {
        cASSERT0(mc, mc->top + 1 == mc->tree->height);
        for (size_t i = 0; i < nkeys; i++) {
          node_t *node = page_node(mp, i);
          if (node_flags(node) & N_BIG) {
            rc = page_retire_ex(mc, node_largedata_pgno(node), nullptr, 0);
            if (unlikely(rc != MDBX_SUCCESS))
              goto bailout;
            if (!(may_have_tables | mc->tree->large_pages))
              goto popup;
          } else if (node_flags(node) & N_TREE) {
            if (unlikely((node_flags(node) & N_DUP) == 0)) {
              rc = /* disallowing implicit table deletion */ MDBX_INCOMPATIBLE;
              goto bailout;
            }
            rc = cursor_dupsort_setup(mc, node, mp);
            if (unlikely(rc != MDBX_SUCCESS))
              goto bailout;
            rc = tree_drop(&mc->subcur->cursor, false);
            if (unlikely(rc != MDBX_SUCCESS))
              goto bailout;
          }
        }
      } else {
        cASSERT0(mc, mc->top + 1 < mc->tree->height);
        mc->checking |= z_retiring;
        const unsigned pagetype = (is_frozen(txn, mp) ? P_FROZEN : 0) +
                                  ((mc->top + 2 == mc->tree->height) ? (mc->checking & (P_LEAF | P_DUPFIX)) : P_BRANCH);
        for (size_t i = 0; i < nkeys; i++) {
          node_t *node = page_node(mp, i);
          cASSERT0(mc, (node_flags(node) & (N_BIG | N_TREE | N_DUP)) == 0);
          const pgno_t pgno = node_pgno(node);
          rc = page_retire_ex(mc, pgno, nullptr, pagetype);
          if (unlikely(rc != MDBX_SUCCESS))
            goto bailout;
        }
        mc->checking -= z_retiring;
      }
      if (!mc->top)
        break;
      cASSERT0(mc, nkeys > 0);
      mc->ki[mc->top] = (indx_t)nkeys;
      rc = cursor_sibling_right(mc);
      if (unlikely(rc != MDBX_SUCCESS)) {
        if (unlikely(rc != MDBX_NOTFOUND))
          goto bailout;
        /* no more siblings, go back to beginning of previous level. */
      popup:
        cursor_pop(mc);
        mc->ki[0] = 0;
        for (intptr_t i = 1; i <= mc->top; i++) {
          mc->pg[i] = stack[i];
          mc->ki[i] = 0;
        }
      }
    }
    rc = page_retire(mc, mc->pg[0]);
  }

bailout:
  be_poor(mc);
  return rc;
}

static int node_move(MDBX_cursor *csrc, MDBX_cursor *cdst, bool fromleft) {
  int rc;
  DKBUF_DEBUG;

  page_t *psrc = csrc->pg[csrc->top];
  page_t *pdst = cdst->pg[cdst->top];
  cASSERT0(csrc, page_type(psrc) == page_type(pdst));
  cASSERT0(csrc, csrc->tree == cdst->tree);
  cASSERT0(csrc, csrc->top == cdst->top);
  if (unlikely(page_type(psrc) != page_type(pdst))) {
  bailout:
    ERROR("Wrong or mismatch pages's types (src %d, dst %d) to move node", page_type(psrc), page_type(pdst));
    csrc->txn->flags |= MDBX_TXN_ERROR;
    return MDBX_PROBLEM;
  }

  MDBX_val key4move;
  switch (page_type(psrc)) {
  case P_BRANCH: {
    const node_t *srcnode = page_node(psrc, csrc->ki[csrc->top]);
    cASSERT0(csrc, node_flags(srcnode) == 0);
    const pgno_t srcpg = node_pgno(srcnode);
    key4move.iov_len = node_ks(srcnode);
    key4move.iov_base = node_key(srcnode);

    if (csrc->ki[csrc->top] == 0) {
      const int8_t top = csrc->top;
      cASSERT0(csrc, top >= 0);
      /* must find the lowest key below src */
      rc = tree_search_lowest(csrc);
      page_t *lowest_page = csrc->pg[csrc->top];
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      cASSERT0(csrc, is_leaf(lowest_page));
      if (unlikely(!is_leaf(lowest_page)))
        goto bailout;
      if (is_dupfix_leaf(lowest_page))
        key4move = page_dupfix_key(lowest_page, 0, csrc->tree->dupfix_size);
      else {
        const node_t *lowest_node = page_node(lowest_page, 0);
        key4move.iov_len = node_ks(lowest_node);
        key4move.iov_base = node_key(lowest_node);
      }

      /* restore cursor after mdbx_page_search_lowest() */
      csrc->top = top;
      csrc->ki[csrc->top] = 0;

      /* paranoia */
      cASSERT0(csrc, psrc == csrc->pg[csrc->top]);
      cASSERT0(csrc, is_branch(psrc));
      if (unlikely(!is_branch(psrc)))
        goto bailout;
    }

    if (cdst->ki[cdst->top] == 0) {
      cursor_couple_t couple;
      MDBX_cursor *const mn = cursor_clone(cdst, &couple);
      const int8_t top = cdst->top;
      cASSERT0(csrc, top >= 0);

      /* must find the lowest key below dst */
      rc = tree_search_lowest(mn);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      page_t *const lowest_page = mn->pg[mn->top];
      cASSERT0(cdst, is_leaf(lowest_page));
      if (unlikely(!is_leaf(lowest_page)))
        goto bailout;
      MDBX_val key;
      if (is_dupfix_leaf(lowest_page))
        key = page_dupfix_key(lowest_page, 0, mn->tree->dupfix_size);
      else {
        node_t *lowest_node = page_node(lowest_page, 0);
        key.iov_len = node_ks(lowest_node);
        key.iov_base = node_key(lowest_node);
      }

      /* restore cursor after mdbx_page_search_lowest() */
      mn->top = top;
      mn->ki[mn->top] = 0;

      const intptr_t delta = EVEN_CEIL(key.iov_len) - EVEN_CEIL(node_ks(page_node(mn->pg[mn->top], 0)));
      const intptr_t needed = branch_size(cdst->txn->env, &key4move) + delta;
      const intptr_t have = page_room(pdst);
      if (unlikely(needed > have))
        return MDBX_RESULT_TRUE;

      if (unlikely((rc = page_touch(csrc)) || (rc = page_touch(cdst))))
        return rc;
      psrc = csrc->pg[csrc->top];
      pdst = cdst->pg[cdst->top];

      couple.outer.next = mn->txn->cursors[cursor_dbi(mn)];
      mn->txn->cursors[cursor_dbi(mn)] = &couple.outer;
      rc = tree_propagate_key(mn, &key);
      mn->txn->cursors[cursor_dbi(mn)] = couple.outer.next;
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    } else {
      const size_t needed = branch_size(cdst->txn->env, &key4move);
      const size_t have = page_room(pdst);
      if (unlikely(needed > have))
        return MDBX_RESULT_TRUE;

      if (unlikely((rc = page_touch(csrc)) || (rc = page_touch(cdst))))
        return rc;
      psrc = csrc->pg[csrc->top];
      pdst = cdst->pg[cdst->top];
    }

    DEBUG("moving %s-node %u [%s] on page %" PRIaPGNO " to node %u on page %" PRIaPGNO, "branch", csrc->ki[csrc->top],
          DKEY_DEBUG(&key4move), psrc->pgno, cdst->ki[cdst->top], pdst->pgno);
    /* Add the node to the destination page. */
    rc = node_add_branch(cdst, cdst->ki[cdst->top], &key4move, srcpg);
  } break;

  case P_LEAF: {
    /* Mark src and dst as dirty. */
    if (unlikely((rc = page_touch(csrc)) || (rc = page_touch(cdst))))
      return rc;
    psrc = csrc->pg[csrc->top];
    pdst = cdst->pg[cdst->top];
    const node_t *srcnode = page_node(psrc, csrc->ki[csrc->top]);
    MDBX_val data;
    data.iov_len = node_ds(srcnode);
    data.iov_base = node_data(srcnode);
    key4move.iov_len = node_ks(srcnode);
    key4move.iov_base = node_key(srcnode);
    DEBUG("moving %s-node %u [%s] on page %" PRIaPGNO " to node %u on page %" PRIaPGNO, "leaf", csrc->ki[csrc->top],
          DKEY_DEBUG(&key4move), psrc->pgno, cdst->ki[cdst->top], pdst->pgno);
    /* Add the node to the destination page. */
    rc = node_add_leaf(cdst, cdst->ki[cdst->top], &key4move, &data, node_flags(srcnode));
  } break;

  case P_LEAF | P_DUPFIX: {
    /* Mark src and dst as dirty. */
    if (unlikely((rc = page_touch(csrc)) || (rc = page_touch(cdst))))
      return rc;
    psrc = csrc->pg[csrc->top];
    pdst = cdst->pg[cdst->top];
    key4move = page_dupfix_key(psrc, csrc->ki[csrc->top], csrc->tree->dupfix_size);
    DEBUG("moving %s-node %u [%s] on page %" PRIaPGNO " to node %u on page %" PRIaPGNO, "leaf2", csrc->ki[csrc->top],
          DKEY_DEBUG(&key4move), psrc->pgno, cdst->ki[cdst->top], pdst->pgno);
    /* Add the node to the destination page. */
    rc = node_add_dupfix(cdst, cdst->ki[cdst->top], &key4move);
  } break;

  default:
    ASSERT(false);
    goto bailout;
  }

  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  /* Delete the node from the source page. */
  node_del(csrc, key4move.iov_len);

  cASSERT0(csrc, psrc == csrc->pg[csrc->top]);
  cASSERT0(cdst, pdst == cdst->pg[cdst->top]);
  cASSERT0(csrc, page_type(psrc) == page_type(pdst));

  /* csrc курсор тут всегда временный, на стеке внутри tree_rebalance(),
   * и его нет необходимости корректировать. */
  {
    /* Adjust other cursors pointing to mp */
    MDBX_cursor *m2, *m3;
    const size_t dbi = cursor_dbi(csrc);
    cASSERT0(csrc, csrc->top == cdst->top);
    if (fromleft) {
      /* Перемещаем с левой страницы нв правую, нужно сдвинуть ki на +1 */
      for (m2 = csrc->txn->cursors[dbi]; m2; m2 = m2->next) {
        m3 = (csrc->flags & z_inner) ? &m2->subcur->cursor : m2;
        if (!is_related(csrc, m3))
          continue;

        if (m3 != cdst && m3->pg[csrc->top] == pdst && m3->ki[csrc->top] >= cdst->ki[csrc->top]) {
          m3->ki[csrc->top] += 1;
        }

        if (/* m3 != csrc && */ m3->pg[csrc->top] == psrc && m3->ki[csrc->top] == csrc->ki[csrc->top]) {
          m3->pg[csrc->top] = pdst;
          m3->ki[csrc->top] = cdst->ki[cdst->top];
          cASSERT0(csrc, csrc->top > 0);
          m3->ki[csrc->top - 1] += 1;
        }

        if (is_leaf(psrc) && inner_pointed(m3)) {
          cASSERT0(csrc, csrc->top == m3->top);
          size_t nkeys = page_numkeys(m3->pg[csrc->top]);
          if (likely(nkeys > m3->ki[csrc->top]))
            cursor_inner_refresh(m3, m3->pg[csrc->top], m3->ki[csrc->top]);
        }
      }
    } else {
      /* Перемещаем с правой страницы на левую, нужно сдвинуть ki на -1 */
      for (m2 = csrc->txn->cursors[dbi]; m2; m2 = m2->next) {
        m3 = (csrc->flags & z_inner) ? &m2->subcur->cursor : m2;
        if (!is_related(csrc, m3))
          continue;
        if (m3->pg[csrc->top] == psrc) {
          if (!m3->ki[csrc->top]) {
            m3->pg[csrc->top] = pdst;
            m3->ki[csrc->top] = cdst->ki[cdst->top];
            cASSERT0(csrc, csrc->top > 0 && m3->ki[csrc->top - 1] > 0);
            m3->ki[csrc->top - 1] -= 1;
          } else
            m3->ki[csrc->top] -= 1;

          if (is_leaf(psrc) && inner_pointed(m3)) {
            cASSERT0(csrc, csrc->top == m3->top);
            size_t nkeys = page_numkeys(m3->pg[csrc->top]);
            if (likely(nkeys > m3->ki[csrc->top]))
              cursor_inner_refresh(m3, m3->pg[csrc->top], m3->ki[csrc->top]);
          }
        }
      }
    }
  }

  /* Update the parent separators. */
  if (csrc->ki[csrc->top] == 0) {
    cASSERT0(csrc, csrc->top > 0);
    if (csrc->ki[csrc->top - 1] != 0) {
      MDBX_val key;
      if (is_dupfix_leaf(psrc))
        key = page_dupfix_key(psrc, 0, csrc->tree->dupfix_size);
      else {
        node_t *srcnode = page_node(psrc, 0);
        key.iov_len = node_ks(srcnode);
        key.iov_base = node_key(srcnode);
      }
      DEBUG("update separator for source page %" PRIaPGNO " to [%s]", psrc->pgno, DKEY_DEBUG(&key));

      cursor_couple_t couple;
      MDBX_cursor *const mn = cursor_clone(csrc, &couple);
      cASSERT0(csrc, mn->top > 0);
      mn->top -= 1;

      couple.outer.next = mn->txn->cursors[cursor_dbi(mn)];
      mn->txn->cursors[cursor_dbi(mn)] = &couple.outer;
      rc = tree_propagate_key(mn, &key);
      mn->txn->cursors[cursor_dbi(mn)] = couple.outer.next;
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    }
    if (is_branch(psrc)) {
      const MDBX_val nullkey = {0, 0};
      const indx_t ix = csrc->ki[csrc->top];
      csrc->ki[csrc->top] = 0;
      rc = tree_propagate_key(csrc, &nullkey);
      csrc->ki[csrc->top] = ix;
      cASSERT0(csrc, rc == MDBX_SUCCESS);
    }
  }

  if (cdst->ki[cdst->top] == 0) {
    cASSERT0(cdst, cdst->top > 0);
    if (cdst->ki[cdst->top - 1] != 0) {
      MDBX_val key;
      if (is_dupfix_leaf(pdst))
        key = page_dupfix_key(pdst, 0, cdst->tree->dupfix_size);
      else {
        node_t *srcnode = page_node(pdst, 0);
        key.iov_len = node_ks(srcnode);
        key.iov_base = node_key(srcnode);
      }
      DEBUG("update separator for destination page %" PRIaPGNO " to [%s]", pdst->pgno, DKEY_DEBUG(&key));
      cursor_couple_t couple;
      MDBX_cursor *const mn = cursor_clone(cdst, &couple);
      cASSERT0(cdst, mn->top > 0);
      mn->top -= 1;

      couple.outer.next = mn->txn->cursors[cursor_dbi(mn)];
      mn->txn->cursors[cursor_dbi(mn)] = &couple.outer;
      rc = tree_propagate_key(mn, &key);
      mn->txn->cursors[cursor_dbi(mn)] = couple.outer.next;
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
    }
    if (is_branch(pdst)) {
      const MDBX_val nullkey = {0, 0};
      const indx_t ix = cdst->ki[cdst->top];
      cdst->ki[cdst->top] = 0;
      rc = tree_propagate_key(cdst, &nullkey);
      cdst->ki[cdst->top] = ix;
      cASSERT0(cdst, rc == MDBX_SUCCESS);
    }
  }

  return MDBX_SUCCESS;
}

static int page_merge(MDBX_cursor *csrc, MDBX_cursor *cdst) {
  MDBX_val key;
  int rc;

  cASSERT0(csrc, csrc != cdst);
  cASSERT1(csrc, cursor_is_tracked(csrc));
  cASSERT1(cdst, cursor_is_tracked(cdst));
  const page_t *const psrc = csrc->pg[csrc->top];
  page_t *pdst = cdst->pg[cdst->top];
  DEBUG("merging page %" PRIaPGNO " into %" PRIaPGNO, psrc->pgno, pdst->pgno);

  cASSERT0(csrc, page_type(psrc) == page_type(pdst));
  cASSERT0(csrc, csrc->clc == cdst->clc && csrc->tree == cdst->tree);
  cASSERT0(csrc, csrc->top > 0); /* can't merge root page */
  cASSERT0(cdst, cdst->top > 0);
  cASSERT0(cdst, cdst->top + 1 < cdst->tree->height || is_leaf(cdst->pg[cdst->tree->height - 1]));
  cASSERT0(csrc, csrc->top + 1 < csrc->tree->height || is_leaf(csrc->pg[csrc->tree->height - 1]));
  cASSERT0(cdst, cursor_dbi(csrc) == FREE_DBI || csrc->txn->env->options.prefer_waf_insteadof_balance ||
                     page_room(pdst) >= page_used(cdst->txn->env, psrc));
  const int pagetype = page_type(psrc);

  /* Move all nodes from src to dst */
  const size_t dst_nkeys = page_numkeys(pdst);
  const size_t src_nkeys = page_numkeys(psrc);
  cASSERT0(cdst, dst_nkeys + src_nkeys >= (is_leaf(psrc) ? 1u : 2u));
  if (likely(src_nkeys)) {
    size_t ii = dst_nkeys;
    if (unlikely(pagetype & P_DUPFIX)) {
      /* Mark dst as dirty. */
      rc = page_touch(cdst);
      cASSERT0(cdst, rc != MDBX_RESULT_TRUE);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;

      key.iov_len = csrc->tree->dupfix_size;
      key.iov_base = page2payload(psrc);
      size_t i = 0;
      do {
        rc = node_add_dupfix(cdst, ii++, &key);
        cASSERT0(cdst, rc != MDBX_RESULT_TRUE);
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;
        key.iov_base = ptr_disp(key.iov_base, key.iov_len);
      } while (++i != src_nkeys);
    } else {
      node_t *srcnode = page_node(psrc, 0);
      key.iov_len = node_ks(srcnode);
      key.iov_base = node_key(srcnode);
      if (pagetype & P_BRANCH) {
        cursor_couple_t couple;
        MDBX_cursor *const mn = cursor_clone(csrc, &couple);

        /* must find the lowest key below src */
        rc = tree_search_lowest(mn);
        cASSERT0(csrc, rc != MDBX_RESULT_TRUE);
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;

        const page_t *mp = mn->pg[mn->top];
        if (likely(!is_dupfix_leaf(mp))) {
          cASSERT0(mn, is_leaf(mp));
          const node_t *lowest = page_node(mp, 0);
          key.iov_len = node_ks(lowest);
          key.iov_base = node_key(lowest);
        } else {
          cASSERT0(mn, mn->top > csrc->top);
          key = page_dupfix_key(mp, mn->ki[mn->top], csrc->tree->dupfix_size);
        }
        cASSERT0(mn, key.iov_len >= csrc->clc->k.lmin);
        cASSERT0(mn, key.iov_len <= csrc->clc->k.lmax);

        const size_t dst_room = page_room(pdst);
        const size_t src_used = page_used(cdst->txn->env, psrc);
        const size_t space_needed = src_used - node_ks(srcnode) + key.iov_len;
        if (unlikely(space_needed > dst_room))
          return MDBX_RESULT_TRUE;
      }

      /* Mark dst as dirty. */
      rc = page_touch(cdst);
      cASSERT0(cdst, rc != MDBX_RESULT_TRUE);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;

      size_t i = 0;
      while (true) {
        if (pagetype & P_LEAF) {
          MDBX_val data;
          data.iov_len = node_ds(srcnode);
          data.iov_base = node_data(srcnode);
          rc = node_add_leaf(cdst, ii++, &key, &data, node_flags(srcnode));
        } else {
          cASSERT0(csrc, node_flags(srcnode) == 0);
          rc = node_add_branch(cdst, ii++, &key, node_pgno(srcnode));
        }
        cASSERT0(cdst, rc != MDBX_RESULT_TRUE);
        if (unlikely(rc != MDBX_SUCCESS))
          return rc;

        if (++i == src_nkeys)
          break;
        srcnode = page_node(psrc, i);
        key.iov_len = node_ks(srcnode);
        key.iov_base = node_key(srcnode);
      }
    }

    pdst = cdst->pg[cdst->top];
    DEBUG("dst page %" PRIaPGNO " now has %zu keys (%u.%u%% filled)", pdst->pgno, page_numkeys(pdst),
          page_fill_percentum_x10(cdst->txn->env, pdst) / 10, page_fill_percentum_x10(cdst->txn->env, pdst) % 10);

    cASSERT0(csrc, psrc == csrc->pg[csrc->top]);
    cASSERT0(cdst, pdst == cdst->pg[cdst->top]);
  }

  /* Unlink the src page from parent and add to free list. */
  csrc->top -= 1;
  node_del(csrc, 0);
  if (csrc->ki[csrc->top] == 0) {
    const MDBX_val nullkey = {0, 0};
    rc = tree_propagate_key(csrc, &nullkey);
    cASSERT0(csrc, rc != MDBX_RESULT_TRUE);
    if (unlikely(rc != MDBX_SUCCESS)) {
      csrc->top += 1;
      return rc;
    }
  }
  csrc->top += 1;

  cASSERT0(csrc, psrc == csrc->pg[csrc->top]);
  cASSERT0(cdst, pdst == cdst->pg[cdst->top]);

  {
    /* Adjust other cursors pointing to mp */
    MDBX_cursor *m2, *m3;
    const size_t dbi = cursor_dbi(csrc);
    for (m2 = csrc->txn->cursors[dbi]; m2; m2 = m2->next) {
      m3 = (csrc->flags & z_inner) ? &m2->subcur->cursor : m2;
      if (!is_related(csrc, m3))
        continue;
      if (m3->pg[csrc->top] == psrc) {
        m3->pg[csrc->top] = pdst;
        m3->ki[csrc->top] += (indx_t)dst_nkeys;
        m3->ki[csrc->top - 1] = cdst->ki[csrc->top - 1];
      } else if (m3->pg[csrc->top - 1] == csrc->pg[csrc->top - 1] && m3->ki[csrc->top - 1] > csrc->ki[csrc->top - 1]) {
        cASSERT0(m3, m3->ki[csrc->top - 1] > 0 && m3->ki[csrc->top - 1] <= page_numkeys(m3->pg[csrc->top - 1]));
        m3->ki[csrc->top - 1] -= 1;
      }

      if (is_leaf(psrc) && inner_pointed(m3)) {
        cASSERT0(csrc, csrc->top == m3->top);
        size_t nkeys = page_numkeys(m3->pg[csrc->top]);
        if (likely(nkeys > m3->ki[csrc->top]))
          cursor_inner_refresh(m3, m3->pg[csrc->top], m3->ki[csrc->top]);
      }
    }
  }

  rc = page_retire(csrc, (page_t *)psrc);
  cASSERT0(csrc, rc != MDBX_RESULT_TRUE);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  cASSERT0(cdst, cdst->tree->items > 0);
  cASSERT0(cdst, cdst->top + 1 <= cdst->tree->height);
  cASSERT0(cdst, cdst->top > 0);
  page_t *const top_page = cdst->pg[cdst->top];
  const indx_t top_indx = cdst->ki[cdst->top];
  const int save_top = cdst->top;
  const uint16_t save_height = cdst->tree->height;
  cursor_pop(cdst);
  rc = tree_rebalance(cdst);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  cASSERT0(cdst, cdst->tree->items > 0);
  cASSERT0(cdst, cdst->top + 1 <= cdst->tree->height);
  if (MDBX_ENABLE_PGOP_STAT)
    cdst->txn->env->lck->pgops.merge.weak += 1;

  if (is_leaf(cdst->pg[cdst->top])) {
    /* LY: don't touch cursor if top-page is a LEAF */
    cASSERT0(cdst, is_leaf(cdst->pg[cdst->top]) || page_type(cdst->pg[cdst->top]) == pagetype);
    return MDBX_SUCCESS;
  }

  cASSERT0(cdst, page_numkeys(top_page) == dst_nkeys + src_nkeys);

  if (unlikely(pagetype != page_type(top_page))) {
    /* LY: LEAF-page becomes BRANCH, unable restore cursor's stack */
    goto bailout;
  }

  if (top_page == cdst->pg[cdst->top]) {
    /* LY: don't touch cursor if prev top-page already on the top */
    cASSERT0(cdst, cdst->ki[cdst->top] == top_indx);
    cASSERT0(cdst, is_leaf(cdst->pg[cdst->top]) || page_type(cdst->pg[cdst->top]) == pagetype);
    return MDBX_SUCCESS;
  }

  const int new_top = save_top - save_height + cdst->tree->height;
  if (unlikely(new_top < 0 || new_top >= cdst->tree->height)) {
    /* LY: out of range, unable restore cursor's stack */
    goto bailout;
  }

  if (top_page == cdst->pg[new_top]) {
    cASSERT0(cdst, cdst->ki[new_top] == top_indx);
    /* LY: restore cursor stack */
    cdst->top = (int8_t)new_top;
    cASSERT0(cdst, cdst->top + 1 < cdst->tree->height || is_leaf(cdst->pg[cdst->tree->height - 1]));
    cASSERT0(cdst, is_leaf(cdst->pg[cdst->top]) || page_type(cdst->pg[cdst->top]) == pagetype);
    return MDBX_SUCCESS;
  }

  page_t *const stub_page = (page_t *)(~(uintptr_t)top_page);
  const indx_t stub_indx = top_indx;
  if (save_height > cdst->tree->height && ((cdst->pg[save_top] == top_page && cdst->ki[save_top] == top_indx) ||
                                           (cdst->pg[save_top] == stub_page && cdst->ki[save_top] == stub_indx))) {
    /* LY: restore cursor stack */
    cdst->pg[new_top] = top_page;
    cdst->ki[new_top] = top_indx;
#if MDBX_DEBUG > 0
    cdst->pg[new_top + 1] = nullptr;
    cdst->ki[new_top + 1] = INT16_MAX;
#endif /* MDBX_DEBUG */
    cdst->top = (int8_t)new_top;
    cASSERT0(cdst, cdst->top + 1 < cdst->tree->height || is_leaf(cdst->pg[cdst->tree->height - 1]));
    cASSERT0(cdst, is_leaf(cdst->pg[cdst->top]) || page_type(cdst->pg[cdst->top]) == pagetype);
    return MDBX_SUCCESS;
  }

bailout:
  /* LY: unable restore cursor's stack */
  be_poor(cdst);
  return MDBX_CURSOR_FULL;
}

int tree_rebalance(MDBX_cursor *mc) {
  cASSERT1(mc, cursor_is_tracked(mc));
  cASSERT0(mc, mc->top >= 0);
  cASSERT0(mc, mc->top + 1 < mc->tree->height || is_leaf(mc->pg[mc->tree->height - 1]));
  const page_t *const tp = mc->pg[mc->top];
  const uint8_t pagetype = page_type(tp);

  STATIC_ASSERT(P_BRANCH == 1);
  const size_t minkeys = (pagetype & P_BRANCH) + (size_t)1;

  /* Pages emptier than this are candidates for merging. */
  size_t room_threshold = mc->txn->env->merge_threshold;
  bool minimize_waf = mc->txn->env->options.prefer_waf_insteadof_balance;
  if (unlikely(mc->tree == &mc->txn->dbs[FREE_DBI])) {
    /* В случае GC всегда минимизируем WAF, а рыхлые страницы объединяем только при наличии запаса в gc_stockpile().
     * Это позволяет уменьшить WAF и избавиться от лишних действий/циклов как при переработке GC,
     * так и при возврате неиспользованных страниц. Сбалансированность b-tree при этом почти не деградирует,
     * ибо добавление/удаление/обновление запиcей происходит почти всегда только по краям. */
    minimize_waf = true;
    room_threshold = page_space(mc->txn->env);
    if (gc_stockpile(mc->txn) > (size_t)mc->tree->height + mc->tree->height)
      room_threshold >>= 1;
  }

  const size_t numkeys = page_numkeys(tp);
  const size_t room = page_room(tp);
  DEBUG("rebalancing %s page %" PRIaPGNO " (has %zu keys, fill %u.%u%%, used %zu, room %zu bytes)",
        is_leaf(tp) ? "leaf" : "branch", tp->pgno, numkeys, page_fill_percentum_x10(mc->txn->env, tp) / 10,
        page_fill_percentum_x10(mc->txn->env, tp) % 10, page_used(mc->txn->env, tp), room);
  cASSERT0(mc, is_modifable(mc->txn, tp));

  if (unlikely(numkeys < minkeys)) {
    DEBUG("page %" PRIaPGNO " must be merged due keys < %zu threshold", tp->pgno, minkeys);
  } else if (unlikely(room > room_threshold)) {
    DEBUG("page %" PRIaPGNO " should be merged due room %zu > %zu threshold", tp->pgno, room, room_threshold);
  } else {
    DEBUG("no need to rebalance page %" PRIaPGNO ", room %zu < %zu threshold", tp->pgno, room, room_threshold);
    cASSERT0(mc, mc->tree->items > 0);
    return MDBX_SUCCESS;
  }

  int rc;
  if (mc->top == 0) {
    page_t *const mp = mc->pg[0];
    const size_t nkeys = page_numkeys(mp);
    cASSERT0(mc, (mc->tree->items == 0) == (nkeys == 0));
    if (nkeys == 0) {
      DEBUG("%s", "tree is completely empty");
      cASSERT0(mc, is_leaf(mp));
      cASSERT0(mc, (*cursor_dbi_state(mc) & DBI_DIRTY) != 0);
      cASSERT0(mc, mc->tree->branch_pages == 0 && mc->tree->large_pages == 0 && mc->tree->leaf_pages == 1);
      /* Adjust cursors pointing to mp */
      for (MDBX_cursor *m2 = mc->txn->cursors[cursor_dbi(mc)]; m2; m2 = m2->next) {
        MDBX_cursor *m3 = (mc->flags & z_inner) ? &m2->subcur->cursor : m2;
        if (!is_poor(m3) && m3->pg[0] == mp) {
          be_poor(m3);
          m3->flags |= z_after_delete;
        }
      }
      if (is_subpage(mp)) {
        return MDBX_SUCCESS;
      } else {
        mc->tree->root = P_INVALID;
        mc->tree->height = 0;
        return page_retire(mc, mp);
      }
    }
    if (is_subpage(mp)) {
      DEBUG("%s", "Can't rebalance a subpage, ignoring");
      cASSERT0(mc, is_leaf(tp));
      return MDBX_SUCCESS;
    }
    if (is_branch(mp) && nkeys == 1) {
      DEBUG("%s", "collapsing root page!");
      mc->tree->root = node_pgno(page_node(mp, 0));
      rc = page_get(mc, mc->tree->root, &mc->pg[0], mp->txnid);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      mc->tree->height--;
      mc->ki[0] = mc->ki[1];
      for (intptr_t i = 1; i < mc->tree->height; i++) {
        mc->pg[i] = mc->pg[i + 1];
        mc->ki[i] = mc->ki[i + 1];
      }

      /* Adjust other cursors pointing to mp */
      for (MDBX_cursor *m2 = mc->txn->cursors[cursor_dbi(mc)]; m2; m2 = m2->next) {
        MDBX_cursor *m3 = (mc->flags & z_inner) ? &m2->subcur->cursor : m2;
        if (is_related(mc, m3) && m3->pg[0] == mp) {
          for (intptr_t i = 0; i < mc->tree->height; i++) {
            m3->pg[i] = m3->pg[i + 1];
            m3->ki[i] = m3->ki[i + 1];
          }
          m3->top -= 1;
        }
      }
      cASSERT0(mc, is_leaf(mc->pg[mc->top]) || page_type(mc->pg[mc->top]) == pagetype);
      cASSERT0(mc, mc->top + 1 < mc->tree->height || is_leaf(mc->pg[mc->tree->height - 1]));
      return page_retire(mc, mp);
    }
    DEBUG("root page %" PRIaPGNO " doesn't need rebalancing (flags 0x%x)", mp->pgno, mp->flags);
    return MDBX_SUCCESS;
  }

  /* The parent (branch page) must have at least 2 pointers,
   * otherwise the tree is invalid. */
  const size_t pre_top = mc->top - 1;
  cASSERT0(mc, is_branch(mc->pg[pre_top]));
  cASSERT0(mc, !is_subpage(mc->pg[0]));
  cASSERT0(mc, page_numkeys(mc->pg[pre_top]) > 1);

  /* Leaf page fill factor is below the threshold.
   * Try to move keys from left or right neighbor, or
   * merge with a neighbor page. */

  /* Find neighbors. */
  cursor_couple_t couple;
  MDBX_cursor *const mn = cursor_clone(mc, &couple);

  page_t *left = nullptr, *right = nullptr;
  if (mn->ki[pre_top] > 0) {
    rc = page_get(mn, node_pgno(page_node(mn->pg[pre_top], mn->ki[pre_top] - 1)), &left, mc->pg[mc->top]->txnid);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    cASSERT0(mc, page_type(left) == page_type(mc->pg[mc->top]));
  }
  if (mn->ki[pre_top] + (size_t)1 < page_numkeys(mn->pg[pre_top])) {
    rc = page_get(mn, node_pgno(page_node(mn->pg[pre_top], mn->ki[pre_top] + (size_t)1)), &right,
                  mc->pg[mc->top]->txnid);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    cASSERT0(mc, page_type(right) == page_type(mc->pg[mc->top]));
  }
  cASSERT0(mc, left || right);

  const size_t ki_top = mc->ki[mc->top];
  const size_t ki_pre_top = mn->ki[pre_top];
  const size_t nkeys = page_numkeys(mn->pg[mn->top]);

  const size_t left_room = left ? page_room(left) : 0;
  const size_t right_room = right ? page_room(right) : 0;
  const size_t left_nkeys = left ? page_numkeys(left) : 0;
  const size_t right_nkeys = right ? page_numkeys(right) : 0;

  /* Нужно выбрать между правой и левой страницами для слияния текущей или перемещения узла в текущую.
   * Таким образом, нужно выбрать один из четырёх вариантов согласно критериям.
   *
   * Если включен minimize_waf, то стараемся не вовлекать чистые страницы,
   * пренебрегая идеальностью баланса ради уменьшения WAF.
   *
   * При этом отдельные варианты могут быть не доступны, либо "не сработать" из-за того что:
   *  - в какой-то branch-странице не хватит места из-за распространения/обновления первых ключей,
   *    которые хранятся в родительских страницах;
   *  - при включенном minimize_waf распространение/обновление первых ключей
   *    потребуется разделение какой-либо страницы, что увеличит WAF и поэтому обесценивает дальнейшее
   *    следование minimize_waf. */

  bool involve = !(left && right);
retry:
  cASSERT0(mc, mc->top > 0);
  const bool consider_left = left && (involve || is_modifable(mc->txn, left));
  const bool consider_right = right && (involve || is_modifable(mc->txn, right));
  if (consider_left && left_room > room_threshold && left_room >= right_room) {
    /* try merge with left */
    cASSERT0(mc, left_nkeys >= minkeys);
    mn->pg[mn->top] = left;
    mn->ki[mn->top - 1] = (indx_t)(ki_pre_top - 1);
    mn->ki[mn->top] = (indx_t)(left_nkeys - 1);
    mc->ki[mc->top] = 0;
    const size_t new_ki = ki_top + left_nkeys;
    mn->ki[mn->top] += mc->ki[mn->top] + 1;
    couple.outer.next = mn->txn->cursors[cursor_dbi(mn)];
    mn->txn->cursors[cursor_dbi(mn)] = &couple.outer;
    rc = page_merge(mc, mn);
    mn->txn->cursors[cursor_dbi(mn)] = couple.outer.next;
    if (likely(rc != MDBX_RESULT_TRUE)) {
      cursor_cpstk(mn, mc);
      mc->ki[mc->top] = (indx_t)new_ki;
      cASSERT0(mc, rc || page_numkeys(mc->pg[mc->top]) >= minkeys);
      return rc;
    }
  }
  if (consider_right && right_room > room_threshold) {
    /* try merge with right */
    cASSERT0(mc, right_nkeys >= minkeys);
    mn->pg[mn->top] = right;
    mn->ki[mn->top - 1] = (indx_t)(ki_pre_top + 1);
    mn->ki[mn->top] = 0;
    mc->ki[mc->top] = (indx_t)nkeys;
    couple.outer.next = mn->txn->cursors[cursor_dbi(mn)];
    mn->txn->cursors[cursor_dbi(mn)] = &couple.outer;
    rc = page_merge(mn, mc);
    mn->txn->cursors[cursor_dbi(mn)] = couple.outer.next;
    if (likely(rc != MDBX_RESULT_TRUE)) {
      mc->ki[mc->top] = (indx_t)ki_top;
      cASSERT0(mc, rc || page_numkeys(mc->pg[mc->top]) >= minkeys);
      return rc;
    }
  }

  if (consider_left && left_nkeys > minkeys && (right_nkeys <= left_nkeys || right_room >= left_room)) {
    /* try move from left */
    mn->pg[mn->top] = left;
    mn->ki[mn->top - 1] = (indx_t)(ki_pre_top - 1);
    mn->ki[mn->top] = (indx_t)(left_nkeys - 1);
    mc->ki[mc->top] = 0;
    couple.outer.next = mn->txn->cursors[cursor_dbi(mn)];
    mn->txn->cursors[cursor_dbi(mn)] = &couple.outer;
    rc = node_move(mn, mc, true);
    mn->txn->cursors[cursor_dbi(mn)] = couple.outer.next;
    if (likely(rc != MDBX_RESULT_TRUE)) {
      mc->ki[mc->top] = (indx_t)(ki_top + 1);
      cASSERT0(mc, rc || page_numkeys(mc->pg[mc->top]) >= minkeys);
      return rc;
    }
  }
  if (consider_right && right_nkeys > minkeys) {
    /* try move from right */
    mn->pg[mn->top] = right;
    mn->ki[mn->top - 1] = (indx_t)(ki_pre_top + 1);
    mn->ki[mn->top] = 0;
    mc->ki[mc->top] = (indx_t)nkeys;
    couple.outer.next = mn->txn->cursors[cursor_dbi(mn)];
    mn->txn->cursors[cursor_dbi(mn)] = &couple.outer;
    rc = node_move(mn, mc, false);
    mn->txn->cursors[cursor_dbi(mn)] = couple.outer.next;
    if (likely(rc != MDBX_RESULT_TRUE)) {
      mc->ki[mc->top] = (indx_t)ki_top;
      cASSERT0(mc, rc || page_numkeys(mc->pg[mc->top]) >= minkeys);
      return rc;
    }
  }

  if (nkeys >= minkeys) {
    mc->ki[mc->top] = (indx_t)ki_top;
    if (CHECKS2_ENABLED())
      ENSURE_OBJ(mc, cursor_validate_updating(mc) == MDBX_SUCCESS);
    return MDBX_SUCCESS;
  }

  if (minimize_waf && room_threshold > 0) {
    /* Если включен minimize_waf, то переходим к попыткам слияния с сильно
     * заполненными страницами до вовлечения чистых страниц (не измененных в этой транзакции) */
    room_threshold = 0;
    goto retry;
  }
  if (!involve) {
    /* Теперь допускаем вовлечение чистых страниц (не измененных в этой транзакции),
     * что улучшает баланс в дереве, но увеличивает WAF. */
    involve = true;
    goto retry;
  }
  if (room_threshold > 0) {
    /* Если не нашли подходящей соседней, то допускаем слияние с сильно заполненными страницами */
    room_threshold = 0;
    goto retry;
  }

  ERROR("Unable to merge/rebalance %s page %" PRIaPGNO " (has %zu keys, fill %u.%u%%, used %zu, room %zu bytes)",
        is_leaf(tp) ? "leaf" : "branch", tp->pgno, numkeys, page_fill_percentum_x10(mc->txn->env, tp) / 10,
        page_fill_percentum_x10(mc->txn->env, tp) % 10, page_used(mc->txn->env, tp), room);
  return MDBX_PROBLEM;
}

int tree_propagate_key(MDBX_cursor *mc, const MDBX_val *key) {
  page_t *mp;
  node_t *node;
  size_t len;
  ptrdiff_t delta, ksize, oksize;
  intptr_t ptr, i, nkeys, indx;
  DKBUF_DEBUG;

  cASSERT0(mc, cursor_is_tracked(mc));
  indx = mc->ki[mc->top];
  mp = mc->pg[mc->top];
  node = page_node(mp, indx);
  ptr = mp->entries[indx];
#if MDBX_DEBUG > 0
  MDBX_val k2;
  k2.iov_base = node_key(node);
  k2.iov_len = node_ks(node);
  DEBUG("update key %zi (offset %zu) [%s] to [%s] on page %" PRIaPGNO, indx, ptr, DVAL_DEBUG(&k2), DKEY_DEBUG(key),
        mp->pgno);
#endif /* MDBX_DEBUG */

  /* Sizes must be 2-byte aligned. */
  ksize = EVEN_CEIL(key->iov_len);
  oksize = EVEN_CEIL(node_ks(node));
  delta = ksize - oksize;

  /* Shift node contents if EVEN_CEIL(key length) changed. */
  if (delta) {
    if (delta > (int)page_room(mp)) {
      /* not enough space left, do a delete and split */
      DEBUG("Not enough room, delta = %zd, splitting...", delta);
      pgno_t pgno = node_pgno(node);
      node_del(mc, 0);
      int err = page_split(mc, key, nullptr, pgno, MDBX_SPLIT_REPLACE);
      if (err == MDBX_SUCCESS && CHECKS2_ENABLED())
        ENSURE_OBJ(mc, cursor_validate_updating(mc) == MDBX_SUCCESS);
      return err;
    }

    nkeys = page_numkeys(mp);
    for (i = 0; i < nkeys; i++) {
      if (mp->entries[i] <= ptr) {
        cASSERT0(mc, mp->entries[i] >= delta);
        mp->entries[i] -= (indx_t)delta;
      }
    }

    void *const base = ptr_disp(mp, mp->upper + PAGEHDRSZ);
    len = ptr - mp->upper + NODESIZE;
    memmove(ptr_disp(base, -delta), base, len);
    cASSERT0(mc, mp->upper >= delta);
    mp->upper -= (indx_t)delta;

    node = page_node(mp, indx);
  }

  /* But even if no shift was needed, update ksize */
  node_set_ks(node, key->iov_len);

  if (likely(key->iov_len /* to avoid UBSAN traps*/ != 0))
    memcpy(node_key(node), key->iov_base, key->iov_len);
  return MDBX_SUCCESS;
}
