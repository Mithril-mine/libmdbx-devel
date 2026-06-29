/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

__hot int page_split(MDBX_cursor *mc, const MDBX_val *const newkey, MDBX_val *const newdata, const pgno_t newpgno,
                     const unsigned naf) {
  page_t *tmp_ki_copy = nullptr;
  MDBX_env *const env = mc->txn->env;
  DKBUF;

  page_t *const mp = mc->pg[mc->top];
  cASSERT0(mc, (mp->flags & P_ILL_BITS) == 0);

  DEBUG(">> splitting %s-page %" PRIaPGNO " and adding %zu+%zu [%s] at %i, nkeys %zi", is_leaf(mp) ? "leaf" : "branch",
        mp->pgno, newkey->iov_len, newdata ? newdata->iov_len : 0, DKEY_DEBUG(newkey), mc->ki[mc->top],
        page_numkeys(mp));

  /* Usually when splitting the root page, the cursor height is 1. But when called from tree_propagate_key(),
   * the cursor height may be greater because it walks up the stack while finding the branch slot to update. */
  intptr_t root_split_height = 0;
  int rc;
  if (mc->top == 0) {
    pgr_t npr = page_new(mc, P_BRANCH);
    rc = npr.err;
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;
    /* shift current top to make room for new parent */
    cASSERT0(mc, mc->tree->height > 0);
#if MDBX_DEBUG > 0
    memset(mc->pg + 3, 0, sizeof(mc->pg) - sizeof(mc->pg[0]) * 3);
    memset(mc->ki + 3, -1, sizeof(mc->ki) - sizeof(mc->ki[0]) * 3);
#endif
    mc->pg[2] = mc->pg[1];
    mc->ki[2] = mc->ki[1];
    mc->pg[1] = mc->pg[0];
    mc->ki[1] = mc->ki[0];
    mc->pg[0] = npr.page;
    mc->ki[0] = 0;
    mc->tree->root = npr.page->pgno;
    DEBUG("root split! new root = %" PRIaPGNO, npr.page->pgno);
    root_split_height = mc->tree->height++;

    /* Add left (implicit) pointer. */
    rc = node_add_branch(mc, 0, nullptr, mp->pgno);
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;

    mc->top = 1;
  } else {
    DEBUG("parent branch page is %" PRIaPGNO, mc->pg[mc->top - 1]->pgno);
  }

  const size_t nkeys = page_numkeys(mp);
  const size_t newindx = mc->ki[mc->top];
  STATIC_ASSERT(P_BRANCH == 1);
  const size_t minkeys = (mp->flags & P_BRANCH) + (size_t)1;
  cASSERT0(mc, nkeys + 1 >= minkeys * 2);
  const size_t leftmost_split = minkeys;
  const size_t rightmost_split = nkeys + 1 - minkeys;

  const size_t reserve_factor = env->options.split_reserve_dot16;
  size_t split_indx = (reserve_factor || env->options.prefer_waf_insteadof_balance) ? newindx : (nkeys + 1) >> 1;

  cASSERT0(mc, !is_branch(mp) || newindx > 0);
  MDBX_val sepkey = {nullptr, 0};
  if (!reserve_factor) {
    if (newindx == nkeys)
      split_indx = rightmost_split;
    else if (newindx <= minkeys) {
      split_indx = leftmost_split;
      /* It is reasonable and possible to split the page at the begin? */
      if (newindx == 0 && !(naf & MDBX_SPLIT_REPLACE)) {
        split_indx = 0;
        /* Checking for ability of splitting by the left-side insertion of a pure page with the new key. */
        for (intptr_t i = mc->top; --i >= 0;)
          if (mc->ki[i]) {
            sepkey = get_key(page_node(mc->pg[i], mc->ki[i]));
            ASSERT(mc->clc->k.cmp(newkey, &sepkey) >= 0);
            goto unable_pure_left;
          }

        /* Save the current first key which was omitted on the parent branch
         * page and should be updated if the new first entry will be added */
        if (is_dupfix_leaf(mp))
          sepkey = page_dupfix_key(mp, 0, mc->tree->dupfix_size);
        else
          sepkey = get_key(page_node(mp, 0));
        ASSERT(mc->clc->k.cmp(newkey, &sepkey) < 0);
        /* Avoiding rare complex cases of nested split the parent page(s) */
        if (page_room(mc->pg[mc->top - 1]) < branch_size(env, &sepkey)) {
          /* TODO: FIXME */
        unable_pure_left:
          split_indx = leftmost_split;
        }
      }
    }
  }
  const bool pure_right = !reserve_factor && split_indx == nkeys;
  const bool pure_left = !reserve_factor && split_indx == 0;

  /* Create a new sibling page. */
  pgr_t npr = page_new(mc, mp->flags);
  rc = npr.err;
  if (unlikely(rc != MDBX_SUCCESS))
    goto done;
  page_t *const sister = npr.page;
  sister->dupfix_ksize = mp->dupfix_ksize;
  DEBUG("new sibling: page %" PRIaPGNO, sister->pgno);
  cursor_couple_t couple;
  MDBX_cursor *const mn = cursor_clone_slightly(mc, &couple);
  mn->pg[mn->top] = sister;
  mn->ki[mn->top] = 0;
  intptr_t prev_top = mc->top - 1;
  mn->ki[prev_top] = mc->ki[prev_top] + 1;

  if (unlikely(pure_right)) {
    /* newindx == split_indx == nkeys */
    TRACE("no-split, but add new pure page at the %s", "right/after");
    cASSERT0(mc, newindx == nkeys && split_indx == nkeys && minkeys == 1);
    sepkey = *newkey;
  } else if (unlikely(pure_left)) {
    /* newindx == split_indx == 0 */
    TRACE("pure-left: no-split, but add new pure page at the %s", "left/before");
    cASSERT0(mc, newindx == 0 && split_indx == 0 && minkeys == 1);
    TRACE("pure-left: old-first-key is %s", DKEY_DEBUG(&sepkey));
  } else {
    split_indx = clamp_unsigned(split_indx, leftmost_split, rightmost_split);
    if (is_dupfix_leaf(sister)) {
      if (reserve_factor) {
        const size_t max_keys_per_page = nkeys;
        const size_t wanna_reserve = max_keys_per_page * reserve_factor >> 16;
        if (wanna_reserve /* + nkeys */ + 1 + wanna_reserve >= max_keys_per_page /* + max_keys_per_page */)
          split_indx = (nkeys + 1) >> 1;
        else {
          const size_t left_space = max_keys_per_page - split_indx - (newindx < split_indx);
          const size_t right_space = split_indx - (newindx >= split_indx);
          ASSERT(left_space + right_space + nkeys + 1 == max_keys_per_page * 2);
          if (wanna_reserve > left_space) {
            ASSERT(left_space < right_space);
            split_indx = max_unsigned(leftmost_split, max_keys_per_page - wanna_reserve -
                                                          (newindx < (max_keys_per_page - wanna_reserve)));
          } else if (wanna_reserve > right_space) {
            ASSERT(left_space > right_space);
            split_indx = min_unsigned(rightmost_split, wanna_reserve + (newindx >= wanna_reserve));
          }
        }
        ASSERT(split_indx >= leftmost_split && split_indx <= rightmost_split);
      }

      /* Move half of the keys to the right sibling */
      ASSERT(newindx == mc->ki[mc->top]);
      const intptr_t distance = newindx - split_indx;
      size_t ksize = mc->tree->dupfix_size;
      void *const split = page_dupfix_ptr(mp, split_indx, ksize);
      size_t rsize = (nkeys - split_indx) * ksize;
      size_t lsize = (nkeys - split_indx) * sizeof(indx_t);
      cASSERT0(mc, mp->lower >= lsize);
      mp->lower -= (indx_t)lsize;
      cASSERT0(mc, sister->lower + lsize <= UINT16_MAX);
      sister->lower += (indx_t)lsize;
      cASSERT0(mc, mp->upper + rsize - lsize <= UINT16_MAX);
      mp->upper += (indx_t)(rsize - lsize);
      cASSERT0(mc, sister->upper >= rsize - lsize);
      sister->upper -= (indx_t)(rsize - lsize);
      sepkey.iov_len = ksize;
      if (distance < 0) {
        cASSERT0(mc, ksize >= sizeof(indx_t));
        void *const ins = page_dupfix_ptr(mp, newindx, ksize);
        sepkey.iov_base = memcpy(sister->entries, split, rsize);
        memmove(ptr_disp(ins, ksize), ins, (split_indx - newindx) * ksize);
        memcpy(ins, newkey->iov_base, ksize);
        cASSERT0(mc, UINT16_MAX - mp->lower >= (int)sizeof(indx_t));
        mp->lower += sizeof(indx_t);
        cASSERT0(mc, mp->upper >= ksize - sizeof(indx_t));
        mp->upper -= (indx_t)(ksize - sizeof(indx_t));
        cASSERT0(mc, (((ksize & page_numkeys(mp)) ^ mp->upper) & 1) == 0);
      } else {
        sepkey.iov_base = (newindx != split_indx) ? split : newkey->iov_base;
        memcpy(sister->entries, split, distance * ksize);
        void *const ins = page_dupfix_ptr(sister, distance, ksize);
        memcpy(ins, newkey->iov_base, ksize);
        memcpy(ptr_disp(ins, ksize), ptr_disp(split, distance * ksize), rsize - distance * ksize);
        cASSERT0(mc, UINT16_MAX - sister->lower >= (int)sizeof(indx_t));
        sister->lower += sizeof(indx_t);
        cASSERT0(mc, sister->upper >= ksize - sizeof(indx_t));
        sister->upper -= (indx_t)(ksize - sizeof(indx_t));
        cASSERT0(mc, distance <= (int)UINT16_MAX);
        mc->ki[mc->top] = (indx_t)distance;
        cASSERT0(mc, (((ksize & page_numkeys(sister)) ^ sister->upper) & 1) == 0);
      }
    } else {
      /* grab a page to hold a temporary copy */
      tmp_ki_copy = page_shadow_alloc(mc->txn, 1);
      if (unlikely(!tmp_ki_copy)) {
        rc = MDBX_ENOMEM;
        goto done;
      }

      /* prepare to insert */
      size_t i = 0;
      while (i < newindx) {
        tmp_ki_copy->entries[i] = mp->entries[i];
        ++i;
      }
      tmp_ki_copy->entries[i] = (indx_t)-1;
      while (++i <= nkeys)
        tmp_ki_copy->entries[i] = mp->entries[i - 1];
      tmp_ki_copy->pgno = mp->pgno;
      tmp_ki_copy->flags = mp->flags;
      tmp_ki_copy->txnid = INVALID_TXNID;
      tmp_ki_copy->lower = 0;
      const size_t max_space = page_space(env);
      tmp_ki_copy->upper = (indx_t)max_space;

      const size_t newsize = is_leaf(mp) ? leaf_size(env, newkey, newdata) : branch_size(env, newkey);
      cASSERT0(mc, split_indx >= leftmost_split && split_indx <= rightmost_split);
      /* Добавляемый узел может не поместиться в страницу-половину вместе
       * с количественной половиной узлов из исходной страницы. В худшем случае,
       * в страницу-половину с добавляемым узлом могут попасть самые больше узлы
       * из исходной страницы, а другую половину только узлы с самыми короткими
       * ключами и с пустыми данными. Поэтому, чтобы найти подходящую границу
       * разреза требуется итерировать узлы подсчитывая объём.
       *
       * Однако, добавляемый узел точно поместится, если его размер не больше
       * чем место "освобождающееся" от заголовков узлов, которые переедут
       * в другую страницу-половину. Плюс, как минимум по одному байту
       * будет в каждом ключе, в худшем случае кроме одного, который может быть
       * нулевого размера. */
      const size_t dim_nodes = (newindx >= split_indx) ? split_indx : nkeys - split_indx;
      const size_t dim_bytes = (sizeof(indx_t) + NODESIZE + 1) * dim_nodes;
      if (reserve_factor || unlikely(newsize >= dim_bytes)) {
        const size_t reserve = reserve_factor ? max_space * reserve_factor >> 16 : 0;
        size_t left_size = 0;
        size_t right_size = newsize + page_used(env, mp);
        size_t best_split = split_indx;
        uint64_t best_skew = UINT64_MAX;
        for (i = 0; i < rightmost_split && left_size < max_space;) {
          size_t size = newsize;
          if (i != newindx) {
            node_t *node = ptr_disp(mp, tmp_ki_copy->entries[i] + PAGEHDRSZ);
            size = NODESIZE + node_ks(node) + sizeof(indx_t);
            if (is_leaf(mp))
              size += (node_flags(node) & N_BIG) ? sizeof(pgno_t) : node_ds(node);
            size = EVEN_CEIL(size);
          }

          left_size += size;
          right_size -= size;
          if (++i >= leftmost_split && left_size <= max_space && right_size <= max_space) {
            const size_t split_skew = branchless_abs(split_indx - i);
            const size_t left_space = max_space - left_size;
            const size_t right_space = max_space - right_size;
            const uint64_t reserve_skew = ((reserve > left_space) ? reserve - left_space : 0) +
                                          ((reserve > right_space) ? reserve - right_space : 0);
            const uint64_t skew = (reserve_skew << 32) | split_skew;
            if (skew >= best_skew)
              break;
            best_split = i;
            best_skew = skew;
          }
        }
        cASSERT0(mc, best_split >= leftmost_split && best_split <= rightmost_split);
        split_indx = best_split;
      }

      sepkey = *newkey;
      if (split_indx != newindx) {
        node_t *node = ptr_disp(mp, tmp_ki_copy->entries[split_indx] + PAGEHDRSZ);
        sepkey.iov_len = node_ks(node);
        sepkey.iov_base = node_key(node);
      }
    }
  }
  DEBUG("separator is %zd [%s]", split_indx, DKEY_DEBUG(&sepkey));

  bool did_split_parent = false;
  /* Copy separator key to the parent. */
  if (unlikely(page_room(mn->pg[prev_top]) < branch_size(env, &sepkey))) {
    TRACE("need split parent branch-page for key %s", DKEY_DEBUG(&sepkey));
    cASSERT0(mc, page_numkeys(mn->pg[prev_top]) > 2);
    cASSERT0(mc, !pure_left);
    const int top = mc->top;
    const int height = mc->tree->height;
    mn->top -= 1;
    did_split_parent = true;
    couple.outer.next = mn->txn->cursors[cursor_dbi(mn)];
    mn->txn->cursors[cursor_dbi(mn)] = &couple.outer;
    rc = page_split(mn, &sepkey, nullptr, sister->pgno, 0);
    mn->txn->cursors[cursor_dbi(mn)] = couple.outer.next;
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;
    cASSERT0(mc, mc->top - top == mc->tree->height - height);
    if (CHECKS2_ENABLED())
      ENSURE_OBJ(mc, cursor_validate_updating(mc) == MDBX_SUCCESS);

    /* root split? */
    prev_top += mc->top - top;
    cASSERT0(mn, prev_top <= mn->top && prev_top <= mc->top);

    /* Right page might now have changed parent.
     * Check if left page also changed parent. */
    if (mn->pg[prev_top] != mc->pg[prev_top] && mc->ki[prev_top] >= page_numkeys(mc->pg[prev_top])) {
      for (intptr_t i = 0; i < prev_top; i++) {
        mc->pg[i] = mn->pg[i];
        mc->ki[i] = mn->ki[i];
      }
      mc->pg[prev_top] = mn->pg[prev_top];
      if (mn->ki[prev_top]) {
        mc->ki[prev_top] = mn->ki[prev_top] - 1;
      } else {
        /* find right page's left sibling */
        mc->ki[prev_top] = mn->ki[prev_top];
        rc = cursor_sibling_left(mc);
        if (unlikely(rc != MDBX_SUCCESS)) {
          if (rc == MDBX_NOTFOUND) /* improper cursor_sibling() result */ {
            ERROR("unexpected %i error going left sibling", rc);
            rc = MDBX_PROBLEM;
          }
          goto done;
        }
      }
    }
  } else if (unlikely(pure_left)) {
    page_t *ptop_page = mc->pg[prev_top];
    TRACE("pure-left: adding to parent page %u node[%u] left-child page #%u key "
          "%s",
          ptop_page->pgno, mc->ki[prev_top], sister->pgno, DKEY(mc->ki[prev_top] ? newkey : nullptr));
    ASSERT(mc->top == prev_top + 1);
    mc->top = (uint8_t)prev_top;
    rc = node_add_branch(mc, mc->ki[prev_top], mc->ki[prev_top] ? newkey : nullptr, sister->pgno);
    cASSERT0(mc, mp == mc->pg[prev_top + 1] && newindx == mc->ki[prev_top + 1] && prev_top == mc->top);

    if (likely(rc == MDBX_SUCCESS) && mc->ki[prev_top] == 0) {
      node_t *node = page_node(mc->pg[prev_top], 1);
      TRACE("pure-left: update prev-first key on parent to %s", DKEY(&sepkey));
      cASSERT0(mc, node_ks(node) == 0 && node_pgno(node) == mp->pgno);
      cASSERT0(mc, mc->top == prev_top && mc->ki[prev_top] == 0);
      mc->ki[prev_top] = 1;
      rc = tree_propagate_key(mc, &sepkey);
      cASSERT0(mc, mc->top == prev_top && mc->ki[prev_top] == 1);
      cASSERT0(mc, mp == mc->pg[prev_top + 1] && newindx == mc->ki[prev_top + 1]);
      mc->ki[prev_top] = 0;
    } else {
      TRACE("pure-left: no-need-update prev-first key on parent %s", DKEY(&sepkey));
    }

    mc->top++;
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;

    node_t *node = page_node(mc->pg[prev_top], mc->ki[prev_top] + (size_t)1);
    cASSERT0(mc, node_pgno(node) == mp->pgno && mc->pg[prev_top] == ptop_page);
  } else {
    mn->top -= 1;
    TRACE("add-to-parent the right-entry[%u] for new sibling-page", mn->ki[prev_top]);
    rc = node_add_branch(mn, mn->ki[prev_top], &sepkey, sister->pgno);
    mn->top += 1;
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;
  }

  if (unlikely(pure_left | pure_right)) {
    mc->pg[mc->top] = sister;
    mc->ki[mc->top] = 0;
    cASSERT0(mc, newpgno == 0 || newpgno == P_INVALID);
    if (is_dupfix_leaf(sister)) {
      cASSERT0(mc, (naf & (N_BIG | N_TREE | N_DUP)) == 0);
      rc = node_add_dupfix(mc, 0, newkey);
    } else {
      cASSERT0(mc, is_leaf(sister));
      rc = node_add_leaf(mc, 0, newkey, newdata, naf);
    }
    if (unlikely(rc != MDBX_SUCCESS))
      goto done;

    if (pure_right) {
      for (intptr_t i = 0; i < mc->top; i++)
        mc->ki[i] = mn->ki[i];
    } else if (mc->ki[mc->top - 1] == 0) {
      for (intptr_t i = 2; i <= mc->top; ++i)
        if (mc->ki[mc->top - i]) {
          sepkey = get_key(page_node(mc->pg[mc->top - i], mc->ki[mc->top - i]));
          if (mc->clc->k.cmp(newkey, &sepkey) < 0) {
            mc->top -= (int8_t)i;
            DEBUG("pure-left: update new-first on parent [%i] page %u key %s", mc->ki[mc->top], mc->pg[mc->top]->pgno,
                  DKEY(newkey));
            rc = tree_propagate_key(mc, newkey);
            mc->top += (int8_t)i;
            if (unlikely(rc != MDBX_SUCCESS))
              goto done;
          }
          break;
        }
    }
  } else if (tmp_ki_copy) { /* !is_dupfix_leaf(mp) */
    /* Move nodes */
    mc->pg[mc->top] = sister;
    size_t n = 0, ii = split_indx;
    do {
      TRACE("i %zu, nkeys %zu => n %zu, rp #%u", ii, nkeys, n, sister->pgno);
      pgno_t pgno = 0;
      unsigned flags = naf;
      MDBX_val rkey, xdata;
      MDBX_val *rdata = nullptr;
      if (unlikely(ii == newindx)) {
        rkey = *newkey;
        if (is_leaf(mp))
          rdata = newdata;
        else
          pgno = newpgno;
        /* Update index for the new key. */
        mc->ki[mc->top] = (indx_t)n;
      } else {
        node_t *node = ptr_disp(mp, tmp_ki_copy->entries[ii] + PAGEHDRSZ);
        rkey.iov_base = node_key(node);
        rkey.iov_len = node_ks(node);
        if (is_leaf(mp)) {
          xdata.iov_base = node_data(node);
          xdata.iov_len = node_ds(node);
          rdata = &xdata;
        } else
          pgno = node_pgno(node);
        flags = node_flags(node);
      }

      if (is_leaf(sister)) {
        cASSERT0(mc, pgno == 0);
        cASSERT0(mc, rdata != nullptr);
        rc = node_add_leaf(mc, n, &rkey, rdata, flags);
      } else {
        cASSERT0(mc, 0 == (uint16_t)flags && is_branch(sister));
        /* First branch index doesn't need key data. */
        rc = node_add_branch(mc, n, n ? &rkey : nullptr, pgno);
      }
      if (unlikely(rc != MDBX_SUCCESS))
        goto done;

      ++n;
      if (++ii > nkeys) {
        ii = 0;
        n = 0;
        mc->pg[mc->top] = tmp_ki_copy;
        TRACE("switch to mp #%u", tmp_ki_copy->pgno);
      }
    } while (ii != split_indx);

    TRACE("ii %zu, nkeys %zu, n %zu, pgno #%u", ii, nkeys, n, mc->pg[mc->top]->pgno);

    n = page_numkeys(tmp_ki_copy);
    for (size_t i = 0; i < n; i++)
      mp->entries[i] = tmp_ki_copy->entries[i];
    mp->lower = tmp_ki_copy->lower;
    mp->upper = tmp_ki_copy->upper;
    memcpy(page_node(mp, n - 1), page_node(tmp_ki_copy, n - 1), env->ps - tmp_ki_copy->upper - PAGEHDRSZ);

    /* reset back to original page */
    if (newindx < split_indx) {
      mc->pg[mc->top] = mp;
    } else {
      mc->pg[mc->top] = sister;
      mc->ki[prev_top]++;
      /* Make sure ki is still valid. */
      if (mn->pg[prev_top] != mc->pg[prev_top] && mc->ki[prev_top] >= page_numkeys(mc->pg[prev_top])) {
        for (intptr_t i = 0; i <= prev_top; i++) {
          mc->pg[i] = mn->pg[i];
          mc->ki[i] = mn->ki[i];
        }
      }
    }
  } else if (newindx >= split_indx) {
    mc->pg[mc->top] = sister;
    mc->ki[prev_top]++;
    /* Make sure ki is still valid. */
    if (mn->pg[prev_top] != mc->pg[prev_top] && mc->ki[prev_top] >= page_numkeys(mc->pg[prev_top])) {
      for (intptr_t i = 0; i <= prev_top; i++) {
        mc->pg[i] = mn->pg[i];
        mc->ki[i] = mn->ki[i];
      }
    }
  }

  /* Adjust other cursors pointing to mp and/or to parent page */
  const size_t n = page_numkeys(mp);
  for (MDBX_cursor *m2 = mc->txn->cursors[cursor_dbi(mc)]; m2; m2 = m2->next) {
    MDBX_cursor *m3 = (mc->flags & z_inner) ? &m2->subcur->cursor : m2;
    if (!is_pointed(m3) || m3 == mc)
      continue;
    if (root_split_height) {
      /* sub cursors may be on different DB */
      if (m3->pg[0] != mp)
        continue;
      /* root split */
      for (intptr_t k = root_split_height; k >= 0; k--) {
        m3->ki[k + 1] = m3->ki[k];
        m3->pg[k + 1] = m3->pg[k];
      }
      m3->ki[0] = m3->ki[0] >= n + pure_left;
      m3->pg[0] = mc->pg[0];
      m3->top += 1;
    }

    if (m3->top >= mc->top && m3->pg[mc->top] == mp && !pure_left) {
      if (m3->ki[mc->top] >= newindx)
        m3->ki[mc->top] += !(naf & MDBX_SPLIT_REPLACE);
      if (m3->ki[mc->top] >= n) {
        m3->pg[mc->top] = sister;
        cASSERT0(mc, m3->ki[mc->top] >= n);
        m3->ki[mc->top] -= (indx_t)n;
        for (intptr_t i = 0; i < mc->top; i++) {
          m3->ki[i] = mn->ki[i];
          m3->pg[i] = mn->pg[i];
        }
      }
    } else if (!did_split_parent && m3->top >= prev_top && m3->pg[prev_top] == mc->pg[prev_top] &&
               m3->ki[prev_top] >= mc->ki[prev_top]) {
      m3->ki[prev_top]++; /* also for the `pure-left` case */
    }
    if (inner_pointed(m3) && is_leaf(mp))
      cursor_inner_refresh(m3, m3->pg[mc->top], m3->ki[mc->top]);
  }
  TRACE("mp #%u left: %zd, sister #%u left: %zd", mp->pgno, page_room(mp), sister->pgno, page_room(sister));

done:
  if (tmp_ki_copy)
    page_shadow_release(env, tmp_ki_copy, 1);

  if (likely(rc == MDBX_SUCCESS)) {
    if (CHECKS2_ENABLED())
      ENSURE_OBJ(mc, cursor_validate_updating(mc) == MDBX_SUCCESS);
    if (unlikely(naf & MDBX_RESERVE)) {
      node_t *node = page_node(mc->pg[mc->top], mc->ki[mc->top]);
      if (!(node_flags(node) & N_BIG))
        newdata->iov_base = node_data(node);
    }
    if (MDBX_ENABLE_PGOP_STAT)
      env->lck->pgops.split.weak += 1;
  } else {
    mc->txn->flags |= MDBX_TXN_ERROR;
    be_poor(mc);
  }

  DEBUG("<< mp #%u, rc %d", mp->pgno, rc);
  return rc;
}
