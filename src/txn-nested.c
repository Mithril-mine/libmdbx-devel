/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

/* Merge pageset of the nested transaction into parent */
static void nested_merge(MDBX_txn *const parent, MDBX_txn *const nested, const size_t parent_retired_len) {
  cASSERT0(nested, (nested->flags & MDBX_WRITEMAP) == 0);
  dpl_t *const src = txn_dpl_sort(nested);

  /* Remove refunded pages from parent's dirty list */
  dpl_t *const dst = txn_dpl_sort(parent);
  if (MDBX_ENABLE_REFUND) {
    size_t n = dst->length;
    while (n && dst->items[n].pgno >= parent->geo.first_unallocated) {
      const unsigned npages = dpl_npages(dst, n);
      page_shadow_release(nested->env, dst->items[n].ptr, npages);
      --n;
    }
    parent->wr.dirtyroom += dst->sorted - n;
    dst->sorted = dpl_setlen(dst, n);
    cASSERT0(parent, parent->wr.dirtyroom + parent->wr.dirtylist->length ==
                         (parent->parent ? parent->parent->wr.dirtyroom : parent->env->options.dp_limit));
  }

  /* Remove reclaimed pages from parent's dirty list.
   * Here the nested->wr.repnl was already moved into parent->wr.repnl
   * and space was reserved via retired_delta. */
  const pnl_t reclaimed_list = parent->wr.repnl;
  txn_dpl_sift(parent, reclaimed_list, false);

  /* Move retired pages from parent's dirty & spilled list to reclaimed */
  size_t r, w, d, s, l;
  for (r = w = parent_retired_len; ++r <= pnl_size(parent->wr.retired_pages);) {
    const pgno_t pgno = parent->wr.retired_pages[r];
    const size_t di = txn_dpl_exist(parent, pgno);
    const size_t si = !di ? spill_search(parent, pgno) : 0;
    unsigned npages;
    const char *kind;
    if (di) {
      page_t *dp = dst->items[di].ptr;
      cASSERT0(parent, (dp->flags & ~(P_LEAF | P_DUPFIX | P_BRANCH | P_LARGE | P_SPILLED)) == 0);
      npages = dpl_npages(dst, di);
      page_wash(parent, di, dp, npages);
      kind = "dirty";
      l = 1;
      if (unlikely(npages > l)) {
        /* OVERFLOW-страница могла быть переиспользована по частям. Тогда
         * в retired-списке может быть только начало последовательности,
         * а остаток растащен по dirty, spilled и reclaimed спискам. Поэтому
         * переносим в reclaimed с проверкой на обрыв последовательности.
         * В любом случае, все осколки будут учтены и отфильтрованы, т.е. если
         * страница была разбита на части, то важно удалить dirty-элемент,
         * а все осколки будут учтены отдельно. */

        /* Список retired страниц не сортирован, но для ускорения сортировки
         * дополняется в соответствии с MDBX_PNL_ASCENDING */
#if MDBX_PNL_ASCENDING
        const size_t len = pnl_size(parent->wr.retired_pages);
        while (r < len && parent->wr.retired_pages[r + 1] == pgno + l) {
          ++r;
          if (++l == npages)
            break;
        }
#else
        while (w > parent_retired_len && parent->wr.retired_pages[w - 1] == pgno + l) {
          --w;
          if (++l == npages)
            break;
        }
#endif
      }
    } else if (unlikely(si)) {
      l = npages = 1;
      spill_remove(parent, si, 1);
      kind = "spilled";
    } else {
      parent->wr.retired_pages[++w] = pgno;
      continue;
    }

    DEBUG("reclaim retired parent's %u -> %zu %s page %" PRIaPGNO, npages, l, kind, pgno);
    /* The space for additions to parent->wr.repnl was already reserverd via the retired_delta. */
    int err = pnl_insert_span(&parent->wr.repnl, pgno, l);
    ENSURE_OBJ(nested, err == MDBX_SUCCESS);
  }
  pnl_setsize(parent->wr.retired_pages, w);

  /* Filter-out parent spill list */
  if (parent->wr.spilled.list && pnl_size(parent->wr.spilled.list) > 0) {
    const pnl_t sl = spill_purge(parent);
    size_t len = pnl_size(sl);
    if (len) {
      /* Remove refunded pages from parent's spill list */
      if (MDBX_ENABLE_REFUND && MDBX_PNL_MOST(sl) >= (parent->geo.first_unallocated << 1)) {
#if MDBX_PNL_ASCENDING
        size_t i = pnl_size(sl);
        ASSERT(MDBX_PNL_MOST(sl) == MDBX_PNL_LAST(sl));
        do {
          if ((sl[i] & 1) == 0)
            DEBUG("refund parent's spilled page %" PRIaPGNO, sl[i] >> 1);
          i -= 1;
        } while (i && sl[i] >= (parent->geo.first_unallocated << 1));
        pnl_setsize(sl, i);
#else
        ASSERT(MDBX_PNL_MOST(sl) == MDBX_PNL_FIRST(sl));
        size_t i = 0;
        do {
          ++i;
          if ((sl[i] & 1) == 0)
            DEBUG("refund parent's spilled page %" PRIaPGNO, sl[i] >> 1);
        } while (i < len && sl[i + 1] >= (parent->geo.first_unallocated << 1));
        pnl_setsize(sl, len -= i);
        memmove(sl + 1, sl + 1 + i, len * sizeof(sl[0]));
#endif
      }
      tASSERT1(nested, pnl_check_allocated(sl, (size_t)parent->geo.first_unallocated << 1));

      /* Remove reclaimed pages from parent's spill list */
      s = pnl_size(sl), r = pnl_size(reclaimed_list);
      /* Scanning from end to begin */
      while (s && r) {
        if (sl[s] & 1) {
          --s;
          continue;
        }
        const pgno_t spilled_pgno = sl[s] >> 1;
        const pgno_t reclaimed_pgno = reclaimed_list[r];
        if (reclaimed_pgno != spilled_pgno) {
          const bool cmp = MDBX_PNL_ORDERED(spilled_pgno, reclaimed_pgno);
          s -= !cmp;
          r -= cmp;
        } else {
          DEBUG("remove reclaimed parent's spilled page %" PRIaPGNO, reclaimed_pgno);
          spill_remove(parent, s, 1);
          --s;
          --r;
        }
      }

      /* Remove anything in our dirty list from parent's spill list */
      /* Scanning spill list in descend order */
      const intptr_t step = MDBX_PNL_ASCENDING ? -1 : 1;
      s = MDBX_PNL_ASCENDING ? pnl_size(sl) : 1;
      d = src->length;
      while (d && (MDBX_PNL_ASCENDING ? s > 0 : s <= pnl_size(sl))) {
        if (sl[s] & 1) {
          s += step;
          continue;
        }
        const pgno_t spilled_pgno = sl[s] >> 1;
        const pgno_t dirty_pgno_form = src->items[d].pgno;
        const unsigned npages = dpl_npages(src, d);
        const pgno_t dirty_pgno_to = dirty_pgno_form + npages;
        if (dirty_pgno_form > spilled_pgno) {
          --d;
          continue;
        }
        if (dirty_pgno_to <= spilled_pgno) {
          s += step;
          continue;
        }

        DEBUG("remove dirtied parent's spilled %u page %" PRIaPGNO, npages, dirty_pgno_form);
        spill_remove(parent, s, 1);
        s += step;
      }

      /* Squash deleted pagenums if we deleted any */
      spill_purge(parent);
    }
  }

  /* Remove anything in our spill list from parent's dirty list */
  if (nested->wr.spilled.list) {
    tASSERT1(nested, pnl_check_allocated(nested->wr.spilled.list, (size_t)parent->geo.first_unallocated << 1));
    txn_dpl_sift(parent, nested->wr.spilled.list, true);
    cASSERT0(parent, parent->wr.dirtyroom + parent->wr.dirtylist->length ==
                         (parent->parent ? parent->parent->wr.dirtyroom : parent->env->options.dp_limit));
  }

  /* Find length of merging our dirty list with parent's and release
   * filter-out pages */
  for (l = 0, d = dst->length, s = src->length; d > 0 && s > 0;) {
    page_t *sp = src->items[s].ptr;
    cASSERT0(parent, (sp->flags & ~(P_LEAF | P_DUPFIX | P_BRANCH | P_LARGE | P_LOOSE | P_SPILLED)) == 0);
    const unsigned s_npages = dpl_npages(src, s);
    const pgno_t s_pgno = src->items[s].pgno;

    page_t *dp = dst->items[d].ptr;
    cASSERT0(parent, (dp->flags & ~(P_LEAF | P_DUPFIX | P_BRANCH | P_LARGE | P_SPILLED)) == 0);
    const unsigned d_npages = dpl_npages(dst, d);
    const pgno_t d_pgno = dst->items[d].pgno;

    if (d_pgno >= s_pgno + s_npages) {
      --d;
      ++l;
    } else if (d_pgno + d_npages <= s_pgno) {
      if (sp->flags != P_LOOSE) {
        sp->txnid = parent->front_txnid;
        sp->flags &= ~P_SPILLED;
      }
      --s;
      ++l;
    } else {
      dst->items[d--].ptr = nullptr;
      page_shadow_release(nested->env, dp, d_npages);
    }
  }
  ASSERT(dst->sorted == dst->length);
  cASSERT0(parent, dst->detent >= l + d + s);
  dst->sorted = l + d + s; /* the merged length */

  while (s > 0) {
    page_t *sp = src->items[s].ptr;
    cASSERT0(parent, (sp->flags & ~(P_LEAF | P_DUPFIX | P_BRANCH | P_LARGE | P_LOOSE | P_SPILLED)) == 0);
    if (sp->flags != P_LOOSE) {
      sp->txnid = parent->front_txnid;
      sp->flags &= ~P_SPILLED;
    }
    --s;
  }

  /* Merge our dirty list into parent's, i.e. merge(dst, src) -> dst */
  if (dst->sorted >= dst->length) {
    /* from end to begin with dst extending */
    for (l = dst->sorted, s = src->length, d = dst->length; s > 0 && d > 0;) {
      if (unlikely(l <= d)) {
        /* squash to get a gap of free space for merge */
        for (r = w = 1; r <= d; ++r)
          if (dst->items[r].ptr) {
            if (w != r) {
              dst->items[w] = dst->items[r];
              dst->items[r].ptr = nullptr;
            }
            ++w;
          }
        VERBOSE("squash to begin for extending-merge %zu -> %zu", d, w - 1);
        d = w - 1;
        continue;
      }
      ASSERT(l > d);
      if (dst->items[d].ptr) {
        dst->items[l--] = (dst->items[d].pgno > src->items[s].pgno) ? dst->items[d--] : src->items[s--];
      } else
        --d;
    }
    if (s > 0) {
      ASSERT(l == s);
      while (d > 0) {
        ASSERT(dst->items[d].ptr == nullptr);
        --d;
      }
      do {
        ASSERT(l > 0);
        dst->items[l--] = src->items[s--];
      } while (s > 0);
    } else {
      ASSERT(l == d);
      while (l > 0) {
        ASSERT(dst->items[l].ptr != nullptr);
        --l;
      }
    }
  } else {
    /* from begin to end with shrinking (a lot of new large/overflow pages) */
    for (l = s = d = 1; s <= src->length && d <= dst->length;) {
      if (unlikely(l >= d)) {
        /* squash to get a gap of free space for merge */
        for (r = w = dst->length; r >= d; --r)
          if (dst->items[r].ptr) {
            if (w != r) {
              dst->items[w] = dst->items[r];
              dst->items[r].ptr = nullptr;
            }
            --w;
          }
        VERBOSE("squash to end for shrinking-merge %zu -> %zu", d, w + 1);
        d = w + 1;
        continue;
      }
      ASSERT(l < d);
      if (dst->items[d].ptr) {
        dst->items[l++] = (dst->items[d].pgno < src->items[s].pgno) ? dst->items[d++] : src->items[s++];
      } else
        ++d;
    }
    if (s <= src->length) {
      ASSERT(dst->sorted - l == src->length - s);
      while (d <= dst->length) {
        ASSERT(dst->items[d].ptr == nullptr);
        --d;
      }
      do {
        ASSERT(l <= dst->sorted);
        dst->items[l++] = src->items[s++];
      } while (s <= src->length);
    } else {
      ASSERT(dst->sorted - l == dst->length - d);
      while (l <= dst->sorted) {
        ASSERT(l <= d && d <= dst->length && dst->items[d].ptr);
        dst->items[l++] = dst->items[d++];
      }
    }
  }
  parent->wr.dirtyroom -= dst->sorted - dst->length;
  ASSERT(parent->wr.dirtyroom <= parent->env->options.dp_limit);
  dpl_setlen(dst, dst->sorted);
  parent->wr.dirtylru = nested->wr.dirtylru;

  /* В текущем понимании выгоднее пересчитать кол-во страниц,
   * чем подмешивать лишние ветвления и вычисления в циклы выше. */
  dst->pages_including_loose = 0;
  for (r = 1; r <= dst->length; ++r)
    dst->pages_including_loose += dpl_npages(dst, r);

  tASSERT1(parent, txn_dpl_check(parent));
  dpl_setlen(src, 0);
  txn_dpl_free(nested);

  if (nested->wr.spilled.list) {
    if (parent->wr.spilled.list) {
      /* Must not fail since space was preserved above. */
      pnl_merge(parent->wr.spilled.list, nested->wr.spilled.list);
      pnl_free(nested->wr.spilled.list);
    } else {
      parent->wr.spilled.list = nested->wr.spilled.list;
      parent->wr.spilled.least_removed = nested->wr.spilled.least_removed;
    }
    tASSERT1(parent, txn_dpl_check(parent));
  }

  if (parent->wr.spilled.list) {
    ASSERT(pnl_check_allocated(parent->wr.spilled.list, (size_t)parent->geo.first_unallocated << 1));
    if (pnl_size(parent->wr.spilled.list))
      parent->flags |= MDBX_TXN_SPILLS;
  }
}

static int nested_start(MDBX_txn *const nested, MDBX_txn *parent) {
  tASSERT1(parent, txn_dpl_check(parent));

  nested->txnid = parent->txnid;
  nested->front_txnid = parent->front_txnid + 1;
  nested->canary = parent->canary;
  parent->flags |= MDBX_TXN_HAS_CHILD;
  parent->nested = nested;
  nested->parent = parent;
  nested->env->txn = nested;
  nested->owner = parent->owner;
  nested->wr.troika = parent->wr.troika;

  const size_t len = pnl_size(parent->wr.repnl) + parent->wr.loose_count;
  cASSERT0(nested, !nested->wr.repnl);
  nested->wr.repnl = pnl_alloc((len > MDBX_PNL_INITIAL) ? len : MDBX_PNL_INITIAL);
  if (unlikely(!nested->wr.repnl))
    return LOG_IFERR(MDBX_ENOMEM);

#if MDBX_ENABLE_DBI_SPARSE
  nested->dbi_sparse = parent->dbi_sparse;
#endif /* MDBX_ENABLE_DBI_SPARSE */
  nested->dbi_seqs = parent->dbi_seqs;
  nested->geo = parent->geo;

  int err = txn_dpl_alloc(nested);
  if (unlikely(err != MDBX_SUCCESS))
    return LOG_IFERR(err);

  /* Move loose pages to reclaimed list */
  if (parent->wr.loose_count) {
    do {
      page_t *lp = parent->wr.loose_pages;
      cASSERT0(parent, lp->flags == P_LOOSE);
      err = pnl_insert_span(&parent->wr.repnl, lp->pgno, 1);
      if (unlikely(err != MDBX_SUCCESS))
        return LOG_IFERR(err);
      MDBX_ASAN_UNPOISON_MEMORY_REGION(&page_next(lp), sizeof(page_t *));
      VALGRIND_MAKE_MEM_DEFINED(&page_next(lp), sizeof(page_t *));
      parent->wr.loose_pages = page_next(lp);
      /* Remove from dirty list */
      page_wash(parent, txn_dpl_exist(parent, lp->pgno), lp, 1);
    } while (parent->wr.loose_pages);
    parent->wr.loose_count = 0;
#if MDBX_ENABLE_REFUND
    parent->wr.loose_refund_wl = 0;
#endif /* MDBX_ENABLE_REFUND */
    tASSERT1(parent, txn_dpl_check(parent));
  }
#if MDBX_ENABLE_REFUND
  nested->wr.loose_refund_wl = 0;
#endif /* MDBX_ENABLE_REFUND */
  nested->wr.dirtyroom = parent->wr.dirtyroom;
  nested->wr.dirtylru = parent->wr.dirtylru;
#if MDBX_ENABLE_PGET_STAT
  nested->ops_pget = 0;
#endif /* MDBX_ENABLE_PGET_STAT */

  txn_dpl_sort(parent);
  if (parent->wr.spilled.list)
    spill_purge(parent);

  cASSERT0(nested, pnl_alloclen(nested->wr.repnl) >= pnl_size(parent->wr.repnl));
  memcpy(nested->wr.repnl, parent->wr.repnl, MDBX_PNL_SIZEOF(parent->wr.repnl));
  /* coverity[ASSERT_SIDE_EFFECT] */
  tASSERT1(nested, pnl_check_allocated(nested->wr.repnl, (nested->geo.first_unallocated /* LY: intentional assignment
                                                                             here, only for assertion */
                                                          = parent->geo.first_unallocated) -
                                                             MDBX_ENABLE_REFUND));

  nested->wr.gc.spent = parent->wr.gc.spent;
  err = rkl_copy(&parent->wr.gc.reclaimed, &nested->wr.gc.reclaimed);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  err = rkl_copy(&parent->wr.gc.ready4reuse, &nested->wr.gc.ready4reuse);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  nested->wr.retired_pages = parent->wr.retired_pages;
  parent->wr.retired_pages = (void *)(intptr_t)pnl_size(parent->wr.retired_pages);
  nested->cursors[FREE_DBI] = nullptr;
  nested->cursors[MAIN_DBI] = nullptr;
  nested->dbi_state[FREE_DBI] = parent->dbi_state[FREE_DBI] & ~(DBI_FRESH | DBI_CREAT | DBI_DIRTY);
  nested->dbi_state[MAIN_DBI] = parent->dbi_state[MAIN_DBI] & ~(DBI_FRESH | DBI_CREAT | DBI_DIRTY);
  memcpy(nested->dbs, parent->dbs, sizeof(nested->dbs[0]) * CORE_DBS);
  if ((nested->n_dbi = parent->n_dbi) > CORE_DBS)
    memset(nested->dbi_state + CORE_DBS, 0, nested->n_dbi - CORE_DBS);

  cASSERT0(parent, parent->wr.dirtyroom + parent->wr.dirtylist->length ==
                       (parent->parent ? parent->parent->wr.dirtyroom : parent->env->options.dp_limit));
  cASSERT0(nested, nested->wr.dirtyroom + nested->wr.dirtylist->length ==
                       (nested->parent ? nested->parent->wr.dirtyroom : nested->env->options.dp_limit));
  return txn_shadow_cursors(parent, MAIN_DBI);
}

int txn_nested_create(MDBX_txn *parent, bool readonly) {
  if (unlikely(parent->flags & MDBX_WRITEMAP)) {
    ERROR("%s mode is incompatible with nested transactions", "MDBX_WRITEMAP");
    return MDBX_INCOMPATIBLE;
  }

  unsigned flags = parent->flags & (MDBX_TXN_SPILLS | MDBX_NOSTICKYTHREADS | MDBX_WRITEMAP);
  if (readonly)
    flags |= txn_ro_nested;

  if (parent->env->options.spill_parent4child_denominator && (flags & txn_ro_nested) == 0) {
    /* Spill dirty-pages of parent to provide dirtyroom for child txn */
    int err =
        txn_spill(parent, nullptr, parent->wr.dirtylist->length / parent->env->options.spill_parent4child_denominator);
    if (unlikely(err != MDBX_SUCCESS))
      return LOG_IFERR(err);
  }
  tASSERT2(parent, audit_ex(parent, 0, false) == 0);

  MDBX_txn *const nested = txn_alloc(flags, parent->env);
  if (unlikely(!nested))
    return LOG_IFERR(MDBX_ENOMEM);

  rkl_init(&nested->wr.gc.comeback);
  int rc = nested_start(nested, parent);
  if (unlikely(rc != MDBX_SUCCESS))
    txn_nested_abort(nested);
  return rc;
}

static int nested_undo(MDBX_txn *nested) {
  cASSERT0(nested, !nested->nested && !(nested->flags & MDBX_TXN_HAS_CHILD));
  cASSERT0(nested, rkl_empty(&nested->wr.gc.comeback));
  if (nested->flags & txn_may_have_cursors)
    txn_done_cursors(nested);
  if (nested->flags & MDBX_TXN_DIRTY)
    dbi_update(nested, false);

  MDBX_txn *const parent = nested->parent;
  if (nested->wr.retired_pages) {
    cASSERT0(parent, pnl_size(nested->wr.retired_pages) >= (uintptr_t)parent->wr.retired_pages);
    pnl_setsize(nested->wr.retired_pages, (uintptr_t)parent->wr.retired_pages);
    parent->wr.retired_pages = nested->wr.retired_pages;
  }

  nested->flags = MDBX_TXN_FINISHED;
  MDBX_env *const env = nested->env;
  if (unlikely(parent->geo.upper != nested->geo.upper || parent->geo.now != nested->geo.now) &&
      !(parent->flags & MDBX_TXN_ERROR) && !(env->flags & ENV_FATAL_ERROR)) {
    /* undo resize performed by nested txn */
    int err = dxb_resize(env, parent->geo.first_unallocated, parent->geo.now, parent->geo.upper, impilict_shrink);
    if (err == MDBX_EPERM) {
      /* unable undo resize (it is regular for Windows),
       * therefore promote size changes from nested to the parent txn */
      WARNING("unable undo resize performed by nested txn, promote to "
              "the parent (%u->%u, %u->%u)",
              nested->geo.upper, parent->geo.now, nested->geo.upper, parent->geo.upper);
      parent->geo.now = nested->geo.upper;
      parent->flags |= MDBX_TXN_DIRTY;
    } else if (unlikely(err != MDBX_SUCCESS)) {
      ERROR("error %d while undo resize performed by nested txn, fail the parent", err);
      mdbx_txn_break(env->basal_txn);
      parent->flags |= MDBX_TXN_ERROR;
      if (!env->dxb_mmap.base)
        env->flags |= ENV_FATAL_ERROR;
    }
  }
  return MDBX_SUCCESS;
}

static void nested_free(MDBX_txn *nested) {
  cASSERT0(nested, !(nested->flags & txn_may_have_cursors));
  MDBX_env *const env = nested->env;
  MDBX_txn *const parent = nested->parent;
  cASSERT0(parent, parent->flags & MDBX_TXN_HAS_CHILD);
  env->txn = parent;
  parent->nested = nullptr;
  parent->flags -= MDBX_TXN_HAS_CHILD;
  nested->signature = 0;
  nested->owner = 0;

  cASSERT0(nested, rkl_empty(&nested->wr.gc.comeback));
  rkl_destroy(&nested->wr.gc.reclaimed);
  rkl_destroy(&nested->wr.gc.ready4reuse);

  txn_dpl_free(nested);
  pnl_free(nested->wr.repnl);
  osal_free(nested);

  tASSERT1(parent, txn_dpl_check(parent));
  tASSERT2(parent, audit_ex(parent, 0, false) == 0);
}

int txn_nested_abort(MDBX_txn *nested) {
  tASSERT0(nested, nested != nested->env->basal_txn);
  tASSERT0(nested, nested->parent->nested == nested && (nested->parent->flags & MDBX_TXN_HAS_CHILD) != 0);
  tASSERT1(nested, txn_dpl_check(nested));
  tASSERT1(nested, pnl_check_allocated(nested->wr.repnl, nested->geo.first_unallocated - MDBX_ENABLE_REFUND));
  tASSERT1(nested, memcmp(&nested->wr.troika, &nested->parent->wr.troika, sizeof(troika_t)) == 0);

  int rc = nested_undo(nested);
  nested_free(nested);
  return rc;
}

static int nested_join(MDBX_txn *nested, struct commit_timestamp *ts) {
  MDBX_env *const env = nested->env;
  MDBX_txn *const parent = nested->parent;
  tASSERT2(nested, audit_ex(nested, 0, false) == 0);
  eASSERT0(env, nested != env->basal_txn);
  eASSERT0(env, parent->nested == nested && (parent->flags & MDBX_TXN_HAS_CHILD) != 0);
  eASSERT1(env, txn_dpl_check(nested));
  tASSERT1(nested, pnl_check_allocated(nested->wr.repnl, nested->geo.first_unallocated - MDBX_ENABLE_REFUND));
  tASSERT1(nested, memcmp(&nested->wr.troika, &parent->wr.troika, sizeof(troika_t)) == 0);
  tASSERT0(nested, rkl_empty(&nested->wr.gc.comeback));

  //-------------------------------------------------------------------------
  // Preserve space for page lists in the parent transaction.

  const size_t parent_retired_len = (uintptr_t)parent->wr.retired_pages;
  if (nested->flags & MDBX_TXN_DIRTY) {
    tASSERT0(nested, parent_retired_len <= pnl_size(nested->wr.retired_pages));
    const size_t retired_delta = pnl_size(nested->wr.retired_pages) - parent_retired_len;
    if (retired_delta) {
      int err = pnl_need(&nested->wr.repnl, retired_delta);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }

    if (nested->wr.spilled.list) {
      if (parent->wr.spilled.list) {
        int err = pnl_need(&parent->wr.spilled.list, pnl_size(nested->wr.spilled.list));
        if (unlikely(err != MDBX_SUCCESS))
          return err;
      }
      spill_purge(nested);
    }

    if (unlikely(nested->wr.dirtylist->length + parent->wr.dirtylist->length > parent->wr.dirtylist->detent &&
                 !txn_dpl_reserve(parent, nested->wr.dirtylist->length + parent->wr.dirtylist->length))) {
      return MDBX_ENOMEM;
    }
  }

  //-------------------------------------------------------------------------
  // No further failures should be allowed.

  if (nested->flags & txn_may_have_cursors)
    /* Merge our cursors into parent's and close them */
    txn_done_cursors(nested);

  parent->wr.retired_pages = nested->wr.retired_pages;
  nested->wr.retired_pages = nullptr;

  pnl_free(parent->wr.repnl);
  parent->wr.repnl = nested->wr.repnl;
  nested->wr.repnl = nullptr;
  parent->wr.gc.spent = nested->wr.gc.spent;
  rkl_destructive_move(&nested->wr.gc.reclaimed, &parent->wr.gc.reclaimed);
  rkl_destructive_move(&nested->wr.gc.ready4reuse, &parent->wr.gc.ready4reuse);
  tASSERT0(nested, rkl_empty(&nested->wr.gc.comeback));

  /* Update parent's DBs array */
  eASSERT0(env, parent->n_dbi == nested->n_dbi);
  TXN_FOREACH_DBI_ALL(nested, dbi) {
    if (nested->dbi_state[dbi] != (parent->dbi_state[dbi] & ~(DBI_FRESH | DBI_CREAT | DBI_DIRTY))) {
      eASSERT0(env, (nested->dbi_state[dbi] & (DBI_CREAT | DBI_FRESH | DBI_DIRTY)) != 0 ||
                        (nested->dbi_state[dbi] | DBI_STALE) ==
                            (parent->dbi_state[dbi] & ~(DBI_FRESH | DBI_CREAT | DBI_DIRTY)));
      parent->dbs[dbi] = nested->dbs[dbi];
      /* preserve parent's status */
      const uint8_t state = nested->dbi_state[dbi] | (parent->dbi_state[dbi] & (DBI_CREAT | DBI_FRESH | DBI_DIRTY));
      DEBUG("dbi %zu dbi-state %s 0x%02x -> 0x%02x", dbi, (parent->dbi_state[dbi] != state) ? "update" : "still",
            parent->dbi_state[dbi], state);
      parent->dbi_state[dbi] = state;
    }
  }

  if (ts)
    ts->prep = osal_monotime();

  if (nested->flags & MDBX_TXN_DIRTY) {
    parent->geo = nested->geo;
    parent->canary = nested->canary;
    parent->flags |= MDBX_TXN_DIRTY;

    /* Move loose pages to parent */
#if MDBX_ENABLE_REFUND
    parent->wr.loose_refund_wl = nested->wr.loose_refund_wl;
#endif /* MDBX_ENABLE_REFUND */
    parent->wr.loose_count = nested->wr.loose_count;
    parent->wr.loose_pages = nested->wr.loose_pages;

    nested_merge(parent, nested, parent_retired_len);

#if MDBX_ENABLE_REFUND
    txn_refund(parent);
    if (CHECKS1_ENABLED()) {
      /* Check parent's loose pages not suitable for refund */
      for (page_t *lp = parent->wr.loose_pages; lp; lp = page_next(lp)) {
        tASSERT0(parent, lp->pgno < parent->wr.loose_refund_wl && lp->pgno + 1 < parent->geo.first_unallocated);
        MDBX_ASAN_UNPOISON_MEMORY_REGION(&page_next(lp), sizeof(page_t *));
        VALGRIND_MAKE_MEM_DEFINED(&page_next(lp), sizeof(page_t *));
      }
      /* Check parent's reclaimed pages not suitable for refund */
      if (pnl_size(parent->wr.repnl))
        tASSERT0(parent, MDBX_PNL_MOST(parent->wr.repnl) + 1 < parent->geo.first_unallocated);
    }
#endif /* MDBX_ENABLE_REFUND */
  } else {
    VERBOSE("fast-complete pure nested txn %" PRIaTXN, nested->txnid);

    tASSERT1(nested, memcmp(&parent->geo, &nested->geo, sizeof(parent->geo)) == 0);
    tASSERT1(nested, memcmp(&parent->canary, &nested->canary, sizeof(parent->canary)) == 0);
    cASSERT0(nested, !nested->wr.spilled.list || pnl_size(nested->wr.spilled.list) == 0);
    cASSERT0(nested, nested->wr.loose_count == 0);
  }

  nested->flags = MDBX_TXN_FINISHED;
  tASSERT2(parent, audit_ex(parent, 0, false) == 0);
  return MDBX_SUCCESS;
}

int txn_nested_commit(MDBX_txn *nested, struct commit_timestamp *ts) {
  int rc = nested_join(nested, ts);
  if (unlikely(rc != MDBX_SUCCESS))
    nested_undo(nested);
  nested_free(nested);
  return rc;
}

int txn_nested_checkpoint(MDBX_txn *nested, struct commit_timestamp *ts) {
  MDBX_txn *parent = nested->parent;
  if (unlikely(!parent || parent->nested != nested || parent->env != nested->env)) {
    ERROR("attempt to %s %s txn %p", "commit", "strange nested", __Wpedantic_format_voidptr(nested));
    return MDBX_PROBLEM;
  }

  unsigned flags = nested->flags & (txn_rw_begin_flags | MDBX_NOSTICKYTHREADS | MDBX_WRITEMAP | MDBX_TXN_RDONLY);
  int rc = nested_join(nested, ts);
  if (likely(rc == MDBX_SUCCESS)) {
    nested->flags = flags | (parent->flags & MDBX_TXN_SPILLS);
    nested->wr.loose_count = 0;
    nested->wr.loose_pages = nullptr;
    rc = nested_start(nested, parent);
    cASSERT0(nested, nested->env->txn == nested);
  }
  if (unlikely(rc != MDBX_SUCCESS))
    txn_nested_abort(nested);
  return rc;
}

MDBX_txn *txn_nested_fakero_begin(MDBX_txn *parent) {
  cASSERT0(parent, (parent->flags & (txn_ro_nested | MDBX_TXN_FINISHED | MDBX_TXN_HAS_CHILD)) == 0);
  if (!MDBX_ENABLE_FAKE_NESTED_READONLY_TRANSACTIONS)
    return nullptr;
  parent->wr.preserve_parent_userctx = parent->userctx;
  parent->flags |= txn_ro_nested;
  return parent;
}

int txn_nested_fakero_end(MDBX_txn *nested) {
  cASSERT0(nested, nested->flags & txn_ro_nested);
  if (!MDBX_ENABLE_FAKE_NESTED_READONLY_TRANSACTIONS)
    return MDBX_BAD_TXN;
  nested->userctx = nested->wr.preserve_parent_userctx;
  nested->flags -= txn_ro_nested;
  return MDBX_SUCCESS;
}

int txn_nested_rollback(MDBX_txn *nested) {
  cASSERT0(nested, nested->flags & txn_ro_nested);
  cASSERT0(nested, rkl_empty(&nested->wr.gc.comeback));
  MDBX_txn *parent = nested->parent;
  if (unlikely(!parent || parent->nested != nested || parent->env != nested->env)) {
    ERROR("attempt to %s %s txn %p", "rollback", "strange nested", __Wpedantic_format_voidptr(nested));
    return MDBX_PROBLEM;
  }

  if (unlikely(F_ISSET(nested->flags, txn_ro_nested | MDBX_TXN_DIRTY))) {
    ERROR("unable to %s %s txn %p", "rollback", "dirty nested fake-readonly", __Wpedantic_format_voidptr(nested));
    return MDBX_BAD_TXN;
  }

  const unsigned preserved_flags = nested->flags & txn_ro_nested;
  int rc = nested_undo(nested);
  if (likely(rc == MDBX_SUCCESS))
    rc = nested_start(nested, parent);
  if (likely(rc == MDBX_SUCCESS))
    nested->flags |= preserved_flags;
  else
    txn_nested_abort(nested);
  return rc;
}
