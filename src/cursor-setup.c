/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file cursor-setup.c — cursor lifecycle: validation, tracking, spill/touch,
/// nested-transaction shadowing, couple init, dupsort setup and cloning;
/// part of the cursor.c module (see also cursor-nav.c / cursor-mut.c /
/// cursor-seek.c and cursor-internal.h).

#include "internals.h"
#include "cursor-internal.h"

__cold int cursor_validate(const MDBX_cursor *mc) {
  if (!mc->txn->wr.dirtylist) {
    cASSERT0(mc, (mc->txn->flags & MDBX_WRITEMAP) != 0 && !MDBX_AVOID_MSYNC);
  } else {
    cASSERT0(mc, (mc->txn->flags & MDBX_WRITEMAP) == 0 || MDBX_AVOID_MSYNC);
    cASSERT0(mc, mc->txn->wr.dirtyroom + mc->txn->wr.dirtylist->length ==
                     (mc->txn->parent ? mc->txn->parent->wr.dirtyroom : mc->txn->env->options.dp_limit));
  }

  cASSERT0(mc, (mc->checking & z_updating) ? mc->top + 1 <= mc->tree->height : mc->top + 1 == mc->tree->height);
  if (unlikely((mc->checking & z_updating) ? mc->top + 1 > mc->tree->height : mc->top + 1 != mc->tree->height))
    return MDBX_CURSOR_FULL;

  if (is_pointed(mc) && (mc->checking & z_updating) == 0) {
    const page_t *mp = mc->pg[mc->top];
    int err = page_check(mc, mp);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    const size_t nkeys = page_numkeys(mp);
    if (!is_hollow(mc)) {
      cASSERT0(mc, mc->ki[mc->top] < nkeys);
      if (mc->ki[mc->top] >= nkeys)
        return MDBX_CURSOR_FULL;
    }
    if (inner_pointed(mc)) {
      cASSERT0(mc, is_filled(mc));
      if (!is_filled(mc))
        return MDBX_CURSOR_FULL;
    }
  }

  for (intptr_t n = 0; n <= mc->top; ++n) {
    page_t *mp = mc->pg[n];
    int err = page_check(mc, mp);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    const size_t nkeys = page_numkeys(mp);
    const bool expect_branch = n < mc->tree->height - 1;
    const bool expect_nested_leaf = n + 1 == mc->tree->height - 1;
    const bool branch = is_branch(mp);
    cASSERT0(mc, branch == expect_branch);
    if (unlikely(branch != expect_branch))
      return MDBX_CURSOR_FULL;
    if ((mc->checking & z_updating) == 0) {
      cASSERT0(mc, nkeys > mc->ki[n] || (!branch && nkeys == mc->ki[n] && (mc->flags & z_hollow) != 0));
      if (unlikely(nkeys <= mc->ki[n] && !(!branch && nkeys == mc->ki[n] && (mc->flags & z_hollow) != 0)))
        return MDBX_CURSOR_FULL;
    } else {
      cASSERT0(mc, nkeys + 1 >= mc->ki[n]);
      if (unlikely(nkeys + 1 < mc->ki[n]))
        return MDBX_CURSOR_FULL;
    }

    for (size_t i = 0; i < nkeys; ++i) {
      if (branch) {
        node_t *node = page_node(mp, i);
        cASSERT0(mc, node_flags(node) == 0);
        if (unlikely(node_flags(node) != 0))
          return MDBX_CURSOR_FULL;
        pgno_t pgno = node_pgno(node);
        page_t *np;
        err = page_get(mc, pgno, &np, mp->txnid);
        cASSERT0(mc, err == MDBX_SUCCESS);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
        const bool nested_leaf = is_leaf(np);
        cASSERT0(mc, nested_leaf == expect_nested_leaf);
        if (unlikely(nested_leaf != expect_nested_leaf))
          return MDBX_CURSOR_FULL;
        err = page_check(mc, np);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
      }
    }
  }
  return MDBX_SUCCESS;
}

__cold int cursor_validate_updating(MDBX_cursor *mc) {
  const uint8_t checking = mc->checking;
  mc->checking |= z_updating;
  const int rc = cursor_validate(mc);
  mc->checking = checking;
  return rc;
}
bool cursor_is_tracked(const MDBX_cursor *mc) {
  for (MDBX_cursor *scan = mc->txn->cursors[cursor_dbi(mc)]; scan; scan = scan->next)
    if (mc == ((mc->flags & z_inner) ? &scan->subcur->cursor : scan))
      return true;
  return false;
}

/*----------------------------------------------------------------------------*/

static int touch_dbi(MDBX_cursor *mc) {
  cASSERT0(mc, (mc->flags & z_inner) == 0);
  cASSERT0(mc, (*cursor_dbi_state(mc) & DBI_DIRTY) == 0);
  *cursor_dbi_state(mc) |= DBI_DIRTY;
  mc->txn->flags |= MDBX_TXN_DIRTY;

  PROBE_AGAINST_DANGLING_TXN(mc->txn);
  if (!cursor_is_core(mc)) {
    /* Touch DB record of named DB */
    cursor_couple_t cx;
    int rc = dbi_check(mc->txn, MAIN_DBI);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    rc = cursor_init(&cx.outer, mc->txn, MAIN_DBI);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    mc->txn->dbi_state[MAIN_DBI] |= DBI_DIRTY;
    cx.outer.next = mc->txn->cursors[MAIN_DBI];
    mc->txn->cursors[MAIN_DBI] = &cx.outer;
    rc = tree_search(&cx.outer, &container_of(mc->clc, kvx_t, clc)->name, Z_MODIFY);
    mc->txn->cursors[MAIN_DBI] = cx.outer.next;
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }
  return MDBX_SUCCESS;
}

__hot int cursor_touch(MDBX_cursor *const mc, const MDBX_val *key, const MDBX_val *data) {
  cASSERT0(mc, (mc->txn->flags & txn_ro_both) == 0);
  cASSERT0(mc, is_pointed(mc) || mc->tree->height == 0);
  cASSERT1(mc, cursor_is_tracked(mc));
  cASSERT0(mc, mc->stash < 1);

  cASSERT0(mc, F_ISSET(dbi_state(mc->txn, FREE_DBI), DBI_LINDO | DBI_VALID));
  cASSERT0(mc, F_ISSET(dbi_state(mc->txn, MAIN_DBI), DBI_LINDO | DBI_VALID));
  PROBE_AGAINST_DANGLING_TXN(mc->txn);
  if (!is_inner(mc)) {
    MDBX_txn *const txn = mc->txn;
    txn_dpl_lru_turn(txn);

    if (unlikely((*cursor_dbi_state(mc) & DBI_DIRTY) == 0)) {
      int err = touch_dbi(mc);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }

    if ((mc->txn->flags & txn_nipped) == 0) {
      /* Estimate how much space this operation will take: */
      /* 1) Max b-tree height, reasonable enough with including dups' sub-tree */
      size_t need = CURSOR_STACK_SIZE + 3;
      /* 2) GC/FreeDB for any payload */
      if (!cursor_is_gc(mc)) {
        need += txn->dbs[FREE_DBI].height + (size_t)3;
        /* 3) Named DBs also dirty the main DB */
        if (!cursor_is_main(mc))
          need += txn->dbs[MAIN_DBI].height + (size_t)3;
      }
#if MDBX_DEBUG_SPILLING != 2
      /* production mode */
      /* 4) Double the page chain estimation
       * for extensively splitting, rebalance and merging */
      need += need;
      /* 5) Factor the key+data which to be put in */
      need += bytes2pgno(txn->env, node_size(key, data)) + (size_t)1;
#else
      /* debug mode */
      (void)key;
      (void)data;
      txn->env->debug_dirtied_est = ++need;
      txn->env->debug_dirtied_act = 0;
#endif /* MDBX_DEBUG_SPILLING == 2 */

      int err = txn_spill(txn, mc, need);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
  }

  if (likely(is_pointed(mc)) && ((mc->txn->flags & MDBX_TXN_SPILLS) || !is_modifiable(mc->txn, mc->pg[mc->top]))) {
    int i = mc->top << CHAR_BIT; /* mc->stash = mc->top; mc->top = 0; */
    do {
      mc->top_and_stash = (uint16_t)i;
      int err = page_touch(mc);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      i += 1 - (1 << CHAR_BIT); /* i.top += 1; i.stash -= 1; */
    } while (i > 0);
    ASSERT(mc->top >= 0 && mc->top < CURSOR_STACK_SIZE && mc->stash == 0);
  }
  return MDBX_SUCCESS;
}

/*----------------------------------------------------------------------------*/

int cursor_shadow(MDBX_cursor *cursor, MDBX_txn *nested, const size_t dbi) {
  tASSERT0(nested, cursor->signature == cur_signature_live);
  tASSERT0(nested, cursor->txn != nested);
  cASSERT0(cursor, cursor->txn->flags & txn_may_have_cursors);
  cASSERT0(cursor, dbi == cursor_dbi(cursor));
  tASSERT0(nested, dbi > FREE_DBI && dbi < nested->n_dbi);

  const size_t size = cursor->subcur ? sizeof(MDBX_cursor) + sizeof(subcur_t) : sizeof(MDBX_cursor);
  MDBX_cursor *const shadow = osal_malloc(size);
  if (unlikely(!shadow))
    return MDBX_ENOMEM;

#if MDBX_DEBUG > 0
  memset(shadow, 0xCD, size);
  VALGRIND_MAKE_MEM_UNDEFINED(shadow, size);
#endif /* MDBX_DEBUG */
  *shadow = *cursor;
  cursor->backup = shadow;
  cursor->txn = nested;
  cursor->tree = &nested->dbs[dbi];
  cursor->dbi_state = &nested->dbi_state[dbi];
  subcur_t *subcur = cursor->subcur;
  if (subcur) {
    *(subcur_t *)(shadow + 1) = *subcur;
    subcur->cursor.txn = nested;
    subcur->cursor.dbi_state = &nested->dbi_state[dbi];
  }
  return MDBX_SUCCESS;
}

MDBX_cursor *cursor_eot(MDBX_cursor *cursor, MDBX_txn *txn) {
  MDBX_cursor *const next = cursor->next;
  const unsigned stage = cursor->signature;
  MDBX_cursor *const shadow = cursor->backup;
  ENSURE_OBJ(txn, stage == cur_signature_live || stage == cur_signature_wait4eot);
  tASSERT0(txn, cursor->txn == txn);
  if (shadow) {
    subcur_t *subcur = cursor->subcur;
    tASSERT0(txn, txn->parent != nullptr && shadow->txn == txn->parent);
    /* Zap: Using uninitialized memory '*subcur->backup'. */
    MDBX_SUPPRESS_GOOFY_MSVC_ANALYZER(6001);
    ENSURE_OBJ(txn, shadow->signature == cur_signature_live);
    tASSERT0(txn, subcur == shadow->subcur);
    if ((txn->flags & MDBX_TXN_ERROR) == 0) {
      /* Update pointers to parent txn */
      cursor->next = shadow->next;
      cursor->backup = shadow->backup;
      cursor->txn = shadow->txn;
      cursor->tree = shadow->tree;
      cursor->dbi_state = shadow->dbi_state;
      if (subcur) {
        subcur->cursor.txn = shadow->txn;
        subcur->cursor.dbi_state = shadow->dbi_state;
      }
    } else {
      /* Restore from backup, i.e. rollback/abort nested txn */
      *cursor = *shadow;
      cursor->signature = stage /* Promote (cur_signature_wait4eot) state to parent txn */;
      if (subcur)
        *subcur = *(subcur_t *)(shadow + 1);
    }
    shadow->signature = 0;
    osal_free(shadow);
  } else {
    cursor_drown((cursor_couple_t *)cursor);
    cursor->next = cursor;
    if (stage == cur_signature_wait4eot) {
      cursor->signature = 0;
      osal_free(cursor);
    } else
      cursor->signature = cur_signature_ready4dispose /* Cursor may be reused */;
  }
  return next;
}

/*----------------------------------------------------------------------------*/

static __always_inline int couple_init(cursor_couple_t *couple, const MDBX_txn *const txn, tree_t *const tree,
                                       kvx_t *const kvx, uint8_t *const dbi_state) {

  VALGRIND_MAKE_MEM_UNDEFINED(couple, sizeof(cursor_couple_t));
  tASSERT0(txn, F_ISSET(*dbi_state, DBI_VALID | DBI_LINDO));

  couple->outer.signature = cur_signature_live;
  couple->outer.next = &couple->outer;
  couple->outer.backup = nullptr;
  couple->outer.txn = (MDBX_txn *)txn;
  couple->outer.tree = tree;
  couple->outer.clc = &kvx->clc;
  couple->outer.dbi_state = dbi_state;
  couple->outer.combo_state = z_fresh_mark;
  STATIC_ASSERT((int)z_branch == P_BRANCH && (int)z_leaf == P_LEAF && (int)z_largepage == P_LARGE &&
                (int)z_dupfix == P_DUPFIX);
  couple->outer.checking = (CHECKS2_ENABLED() || (txn->env->flags & MDBX_VALIDATION)) ? z_pagecheck | z_leaf : z_leaf;
  couple->outer.subcur = nullptr;
#if MDBX_DEBUG_SEARCH_BRANCHLESS
  couple->outer.search_step_counter = 42;
#endif

#if MDBX_DEBUG_SPILLING > 0
  couple->outer.tmp_split_top = 0;
  couple->inner.cursor.tmp_split_top = 0;
#endif /* MDBX_DEBUG_SPILLING */

  subcur_t *const mx = &couple->inner;
  mx->cursor.combo_state = z_fresh_mark | z_inner;
  mx->nested_tree.root = 0;
  if (tree->flags & MDBX_DUPSORT) {
    mx->cursor.signature = cur_signature_live;
    couple->outer.subcur = mx;
    mx->cursor.subcur = nullptr;
    mx->cursor.next = &mx->cursor;
    mx->cursor.txn = (MDBX_txn *)txn;
    mx->cursor.tree = &mx->nested_tree;
    mx->cursor.clc = ptr_disp(couple->outer.clc, sizeof(clc_t));
    tASSERT0(txn, &mx->cursor.clc->k == &kvx->clc.v);
    mx->cursor.dbi_state = dbi_state;
    STATIC_ASSERT(MDBX_DUPFIXED * 2 == P_DUPFIX);
    mx->cursor.checking = couple->outer.checking + ((tree->flags & MDBX_DUPFIXED) << 1);
#if MDBX_DEBUG_SEARCH_BRANCHLESS
    mx->cursor.search_step_counter = 421;
#endif
  }

  if (unlikely(*dbi_state & DBI_STALE))
    return tbl_refresh_absent2baddbi(couple->outer.txn, cursor_dbi(&couple->outer));

  return tbl_setup_ifneed(txn->env, kvx, tree);
}

__cold int cursor_init4walk(cursor_couple_t *couple, const MDBX_txn *const txn, tree_t *const tree, kvx_t *const kvx) {
  return couple_init(couple, txn, tree, kvx, txn->dbi_state);
}

int cursor_init(MDBX_cursor *mc, const MDBX_txn *txn, size_t dbi) {
  STATIC_ASSERT(offsetof(cursor_couple_t, outer) == 0);
  int rc = dbi_check(txn, dbi);
  if (likely(rc == MDBX_SUCCESS))
    rc = couple_init(container_of(mc, cursor_couple_t, outer), txn, &txn->dbs[dbi], &txn->env->kvs[dbi],
                     &txn->dbi_state[dbi]);
  return rc;
}

__cold int unexpected_dupsort(MDBX_cursor *mc) {
  ERROR("unexpected dupsort-page/node for non-dupsort db/cursor (dbi %zu)", cursor_dbi(mc));
  mc->txn->flags |= MDBX_TXN_ERROR;
  be_poor(mc);
  return MDBX_CORRUPTED;
}

int cursor_dupsort_setup(MDBX_cursor *mc, const node_t *node, const page_t *mp) {
  cASSERT0(mc, is_pointed(mc));
  subcur_t *mx = mc->subcur;
  if (!MDBX_DISABLE_VALIDATION && unlikely(mx == nullptr))
    return unexpected_dupsort(mc);

#if MDBX_DEBUG_SPILLING > 0
  mx->cursor.tmp_split_top = 0;
#endif /* MDBX_DEBUG_SPILLING */

  const uint8_t flags = node_flags(node);
  switch (flags) {
  default:
    bad_page(mp, "invalid node flags %u", flags);
    goto bailout;
  case N_DUP | N_TREE:
    if (!MDBX_DISABLE_VALIDATION && unlikely(node_ds(node) != sizeof(tree_t))) {
      ERROR("invalid nested-db record size (%zu, expect %zu)", node_ds(node), sizeof(tree_t));
      goto bailout;
    }
    memcpy(&mx->nested_tree, node_data(node), sizeof(tree_t));
    /* The cursor page/index stacks are fixed-size (CURSOR_STACK_SIZE), so a
     * nested tree whose height is beyond that bound would walk the stacks out
     * of bounds during tree_rebalance() root-collapse. */
    if (!MDBX_DISABLE_VALIDATION && unlikely(mx->nested_tree.height >= CURSOR_STACK_SIZE)) {
      ERROR("nested-db height %u is out of range", mx->nested_tree.height);
      goto bailout;
    }
    const txnid_t pp_txnid = mp->txnid;
    if (!MDBX_DISABLE_VALIDATION && unlikely(mx->nested_tree.mod_txnid > pp_txnid)) {
      ERROR("nested-db.mod_txnid (%" PRIaTXN ") > page-txnid (%" PRIaTXN ")", mx->nested_tree.mod_txnid, pp_txnid);
      goto bailout;
    }
    mx->cursor.combo_state = z_fresh_mark | z_inner;
    break;
  case N_DUP: {
    const size_t dsize = node_ds(node);
    page_t *const sp = node_data(node);
    if (!MDBX_DISABLE_VALIDATION && unlikely(!subpage_geometry_ok(sp, dsize))) {
      /* Crafted nested-page lower/upper make node_del()/cursor_put()
       * underflow their memmove()/memcpy() lengths even on the normal
       * (non-MDBX_VALIDATION) path. (issue #176) */
      return bad_page(sp, "invalid nested %s-page lower(%u)/upper(%u) for size %zu\n",
                      is_dupfix_leaf(sp) ? "leaf2" : "leaf", sp->lower, sp->upper, node_ds(node) - PAGEHDRSZ);
    }
    if (!MDBX_DISABLE_VALIDATION && unlikely(dsize <= PAGEHDRSZ)) {
      ERROR("invalid nested-page size %zu", dsize);
      goto bailout;
    }
    mx->nested_tree.flags = flags_db2sub(mc->tree->flags);
    mx->nested_tree.height = 1;
    mx->nested_tree.dupfix_size = mc->tree->dupfix_size;
    mx->nested_tree.root = 0;
    mx->nested_tree.branch_pages = 0;
    mx->nested_tree.leaf_pages = 1;
    mx->nested_tree.large_pages = 0;
    mx->nested_tree.sequence = 0;
    mx->nested_tree.items = page_numkeys(sp);
    mx->nested_tree.mod_txnid = mp->txnid;
    mx->cursor.combo_state = z_inner;
    mx->cursor.pg[0] = sp;
    mx->cursor.ki[0] = 0;
    mx->nested_tree.flags = flags_db2sub(mc->tree->flags);
    mx->nested_tree.dupfix_size = (mc->tree->flags & MDBX_DUPFIXED) ? sp->dupfix_ksize : 0;
  }
  } /* switch */

  if (unlikely(mx->nested_tree.dupfix_size != mc->tree->dupfix_size)) {
    if (!MDBX_DISABLE_VALIDATION && unlikely(mc->tree->dupfix_size != 0)) {
      ERROR("cursor mismatched nested-db dupfix_size %u", mc->tree->dupfix_size);
      goto bailout;
    }
    if (!MDBX_DISABLE_VALIDATION && unlikely((mc->tree->flags & MDBX_DUPFIXED) == 0)) {
      ERROR("mismatched nested-db flags %u", mc->tree->flags);
      goto bailout;
    }
    if (!MDBX_DISABLE_VALIDATION &&
        unlikely(mx->nested_tree.dupfix_size < mc->clc->v.lmin || mx->nested_tree.dupfix_size > mc->clc->v.lmax)) {
      ERROR("mismatched nested-db.dupfix_size (%u) <> min/max value-length "
            "(%zu/%zu)",
            mx->nested_tree.dupfix_size, mc->clc->v.lmin, mc->clc->v.lmax);
      goto bailout;
    }
    mc->tree->dupfix_size = mx->nested_tree.dupfix_size;
    mc->clc->v.lmin = mc->clc->v.lmax = mx->nested_tree.dupfix_size;
    cASSERT0(mc, mc->clc->v.lmax >= mc->clc->v.lmin);
  }

  DEBUG("Sub-db dbi %zu root page %" PRIaPGNO, cursor_dbi(&mx->cursor), mx->nested_tree.root);
  return MDBX_SUCCESS;

bailout:
  mx->cursor.combo_state = z_poor_mark | z_inner;
  return MDBX_CORRUPTED;
}

/*----------------------------------------------------------------------------*/

MDBX_cursor *cursor_cpstk(const MDBX_cursor *csrc, MDBX_cursor *cdst) {
  cASSERT0(cdst, cdst->txn == csrc->txn);
  cASSERT0(cdst, cdst->clc == csrc->clc);
  cASSERT0(cdst, cdst->dbi_state == csrc->dbi_state);
  cdst->combo_state = csrc->combo_state;
#if MDBX_DEBUG_SPILLING > 0
  cdst->tmp_split_top = 0;
#endif /* MDBX_DEBUG_SPILLING */

  for (intptr_t i = 0, last = csrc->top + csrc->stash; i <= last; i++) {
    cdst->pg[i] = csrc->pg[i];
    cdst->ki[i] = csrc->ki[i];
  }
  return cdst;
}

MDBX_cursor *cursor_copy_position(const MDBX_cursor *csrc, MDBX_cursor *cdst) {
  cASSERT0(cdst, csrc != cdst);
  cursor_cpstk(csrc, cdst);
  if (cdst->subcur) {
    inner_gone_unconditional(cdst);
    ASSERT(csrc->subcur);
    if (is_pointed(&csrc->subcur->cursor)) {
      cursor_cpstk(&csrc->subcur->cursor, &cdst->subcur->cursor);
      *cdst->subcur->cursor.tree = *csrc->subcur->cursor.tree;
    }
  }
  return cdst;
}

static inline void cursor_copy_half(const MDBX_cursor *csrc, MDBX_cursor *cdst) {
  cdst->next = nullptr;
  cdst->backup = nullptr;
  cdst->subcur = nullptr;
  cdst->clc = csrc->clc;
  cdst->txn = csrc->txn;
  cdst->dbi_state = csrc->dbi_state;
  cdst->checking = csrc->checking;
  cdst->tree = csrc->tree;
  cursor_cpstk(csrc, cdst);
}

static inline MDBX_cursor *cursor_clone(bool clone_inner_tree, const MDBX_cursor *csrc, cursor_couple_t *couple) {
  MDBX_cursor *ret = &couple->outer;
  if (is_inner(csrc)) {
    ret = &couple->inner.cursor;
    csrc = cursor_outer((MDBX_cursor *)csrc);
  }

  cursor_copy_half(csrc, &couple->outer);
  if (has_inner(csrc)) {
    couple->outer.subcur = &couple->inner;
    cursor_copy_half(&csrc->subcur->cursor, &couple->inner.cursor);
    if (clone_inner_tree) {
      couple->inner.nested_tree = *container_of(csrc, cursor_couple_t, outer)->inner.cursor.tree;
      couple->inner.cursor.tree = &couple->inner.nested_tree;
    }
  }

  return ret;
}

MDBX_cursor *cursor_clone_share_inner_tree(const MDBX_cursor *csrc, cursor_couple_t *couple) {
  if (is_inner(csrc)) {
    /* Подход копирования только внутреннего курсора не состоятелен (untenable), так как привносит ряд проблем:
     *  - в outer-части курсора в любом случае требуется инициализировать большинство полей,
     *    а экономия получается только на копировании стека;
     *  - для обхода курсоров при подготовке к спиллингу (подсчёта и пометки страниц на которые ссылаются стеки),
     *    требуется обеспечивать не-пустую outer-часть;
     *  - при отслеживании и корректировке состояния курсоров, не-заполненная outer-часть не позволяет во всех случаях
     *    корректно отфильтровать курсоры и обновлять/инвалидировать inner-части.
     */
  }
  return cursor_clone(false, csrc, couple);
}

MDBX_cursor *cursor_clone_complete(const MDBX_cursor *csrc, cursor_couple_t *couple) {
  return cursor_clone(true, csrc, couple);
}
