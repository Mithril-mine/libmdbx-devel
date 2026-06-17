/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

static uint64_t defrag_now(uint64_t now_cache) { return now_cache ? now_cache : osal_monotime(); }

uint64_t defrag_result(dfc_t *dfc, MDBX_defrag_result_t *out, uint64_t now_cache) {
  memset(out, 0, sizeof(*out));
  if (dfc->txn)
    dfc->last_allocated = dfc->txn->geo.first_unallocated;
  out->pages_shrinked = dfc->before_defrag - dfc->last_allocated;
  out->pages_moved = dfc->cycle_pages_moved;
  out->pages_scheduled = dfc->cycle_pages_scheduled;
  out->pages_retained = dfc->gc_retained_pages;
  if (dfc->stopor)
    out->pages_retained += dfc->gc_tree_pages;

  intptr_t pages_left = dfc->last_allocated - (dfc->payload_pages + out->pages_retained);
  out->pages_left = (pages_left > 0) ? pages_left : 0;
  out->pages_whole = dfc->before_defrag;

  size_t denominator = (dfc->txn ? dfc->txn->dbs[FREE_DBI].items + rkl_len(&dfc->txn->wr.gc.ready4reuse) : 0) +
                       (dfc->cycle ? (dfc->last_allocated - dfc->payload_pages) * 2 + dfc->cycle_preprogress
                                   : out->pages_whole - NUM_METAS);
  out->rough_estimation_cycle_progress_permille =
      (dfc->progress_counter < denominator) ? (unsigned)(dfc->progress_counter * UINT64_C(1000) / denominator) : 1000;

  if (MDBX_DEBUG > 0 && dfc->progress_counter > denominator && dfc->txn) {
    WARNING("progress_counter %zu > denominator %zu | gc-items %" PRIu64 ", rkl-ready4reuse %zu | "
            "last_allocated %u, "
            "payload_pages %u, cycle_pages_scheduled %u | pages_whole %zu, walk_cutoff %u",
            dfc->progress_counter, denominator, dfc->txn->dbs[FREE_DBI].items, rkl_len(&dfc->txn->wr.gc.ready4reuse),
            dfc->last_allocated, dfc->payload_pages, dfc->cycle_pages_scheduled, out->pages_whole, dfc->walk_cutoff);
  }

  out->obstructed_pgno = dfc->stumble_pgno;
  out->obstructed_span = dfc->stumble_pgno ? dfc->stumble_span : 0;
  out->obstructed_txnid = dfc->gc_obstacle.txnid;
  out->obstructor_tid = dfc->gc_obstacle.tid;
  out->obstructor_pid = dfc->gc_obstacle.pid;
  out->cycles = dfc->cycle;
  out->stopping_reasons = dfc->stopping_reasons;
  out->spent_time_dot16 =
      osal_monotime_to_16dot16_noUnderflow((now_cache = defrag_now(now_cache)) - dfc->start_timestamp);
  return now_cache;
}

static bool defrag_gc_empty(const dfc_t *dfc) { return dfc->txn->dbs[FREE_DBI].items == 0; }

static uint64_t defrag_notify(dfc_t *dfc) {
  uint64_t now_cache = 0;
  if (dfc->user_callback) {
    MDBX_defrag_result_t progress;
    now_cache = defrag_result(dfc, &progress, now_cache);
    const int feedback = dfc->user_callback(dfc->user_ctx, &progress);
    if (feedback != 0)
      dfc->stopping_reasons |= (feedback > 0) ? MDBX_defrag_discontinued : MDBX_defrag_aborted;
  }
  return now_cache;
}

static void defrag_stumble(dfc_t *dfc, const da_t *at, const char *reason_prefix, ptrdiff_t reason_value,
                           const char *reason_suffix) {
  if (dfc->stumble_pgno < at->key_or_pgno) {
    dfc->stumble_pgno = at->key_or_pgno;
    dfc->stumble_span = at->npages;
  }
  NOTICE("defragmentation cycle %u (moved %+i pages, %+i scheduled), stumbled on the page/chunk %" PRIaPGNO
         "[%u], because %s%zi%s.",
         dfc->cycle, dfc->cycle_pages_moved, dfc->cycle_pages_scheduled, at->key_or_pgno, at->npages, reason_prefix,
         reason_value, reason_suffix);
  defrag_milestone(dfc);
}

void defrag_milestone(dfc_t *dfc) { defrag_notify(dfc); }

static void defrag_progress(dfc_t *dfc, size_t progress_increment) {
  uint64_t now_cache = (progress_increment && ((dfc->progress_counter += progress_increment) & dfc->notify_watchmask))
                           ? 0
                           : defrag_notify(dfc);

  if (dfc->stopping_reasons < MDBX_defrag_discontinued && dfc->txn &&
      dfc->defrag_atleast >= dfc->txn->geo.first_unallocated) {
    if (dfc->defrag_enough >= dfc->txn->geo.first_unallocated) {
      if (!dfc->wallclock_atleast || (now_cache = defrag_now(now_cache)) >= dfc->wallclock_atleast)
        dfc->stopping_reasons |= MDBX_defrag_enough_threshold;
    } else if (dfc->wallclock_detent && ++dfc->wallclock_trottle % 64 == 0 &&
               (now_cache = defrag_now(now_cache)) >= dfc->wallclock_detent)
      dfc->stopping_reasons |= MDBX_defrag_time_limit;
  }
}

static bool defrag_should_continue(dfc_t *dfc, size_t progress_increment) {
  defrag_progress(dfc, progress_increment);
  return !defrag_discontinued(dfc);
}

uint64_t defrag_score(dfc_t *dfc, size_t allocated_pages) {
  MDBX_txn *const txn = dfc->txn;
  const const_pnl_t pnl = txn->wr.repnl;
  const tree_t *gc = &dfc->txn->dbs[FREE_DBI];

  /* Считаем рейтинг "полезности" состояния GC, чтобы избежать бессмысленного обновления/коммита.
   * Состояние однозначно лучше, если уменьшилось количество распределенных страниц или в GC стало больше
   * последовательностей. Используем только reclaiming-список, в который должно быть загружено всё состояние GC. */
  dfc->gc_tree_pages = gc->branch_pages + gc->leaf_pages + gc->large_pages;
  uint64_t score = MAX_PAGENO - allocated_pages;
  score <<= 32;
  score -= (gc->items + gc->height + dfc->gc_tree_pages) << 5;
  for (size_t span, i = 1; i <= pnl_size(pnl); i += span) {
    span = pnl_scan_span(pnl, i);
    span = (span < UINT16_MAX) ? span : UINT16_MAX;
    score += span * span - 2;
  }
  return score;
}

static int defrag_load_gc(dfc_t *dfc) {
  MDBX_val gc_key, gc_data;
  MDBX_txn *const txn = dfc->txn;

  txn_gc_detent(txn);
  dfc->stopor = 0;
  dfc->gc_retained_pages = 0;
  ASSERT((dfc->stopping_reasons & MDBX_defrag_laggard_reader) == 0);

  MDBX_cursor *const gc = gc_cursor_init(txn);
  int rc = outer_first(gc, &gc_key, &gc_data);
  while (rc == MDBX_SUCCESS) {
    if (unlikely(gc_check_keylen(gc_key.iov_len))) {
      ERROR("%s/%d: %s", "corrupted GC-record", rc = MDBX_CORRUPTED, gc_check_keylen(gc_key.iov_len));
      break;
    }
    const glr_t glr = gc_row_pnl(txn, gc_data);
    if (unlikely(glr.err != MDBX_SUCCESS)) {
      ERROR("%s/%d: %s", "corrupted GC-record", rc = glr.err, glr.reason);
      break;
    }
    const txnid_t id = unaligned_peek_u64(4, gc_key.iov_base);
    if (id > txn->env->gc.detent) {
      if (!dfc->stopor) {
        if (id <= txn_basis_snapshot(txn)) {
          if (meta_prefer_steady(txn->env, &txn->wr.troika).txnid == txn->env->gc.detent) {
            /* make steady checkpoint. */
#if MDBX_ENABLE_PROFGC
            txn->env->lck->pgops.gc_prof.flushes += 1;
#endif /* MDBX_ENABLE_PROFGC */
            const meta_ptr_t recent = meta_recent(txn->env, &txn->wr.troika);
            ASSERT(recent.txnid > txn->env->gc.detent);
            meta_t meta = *recent.ptr_c;
            rc = dxb_sync_locked(txn->env, txn->env->flags & (MDBX_WRITEMAP | MDBX_NOMETASYNC), &meta, &txn->wr.troika);
            if (unlikely(rc != MDBX_SUCCESS))
              break;
          }
          if (txn_gc_detent(txn))
            continue;
          if (mvcc_kick_laggards(txn, txn->env->gc.detent, &dfc->gc_obstacle) && txn_gc_detent(txn))
            continue;
        }
        dfc->stopor = id;
        dfc->stopping_reasons |= MDBX_defrag_laggard_reader;
      }
      dfc->gc_retained_pages += pnl_size(glr.pnl);
    } else if (!gc_is_reclaimed(txn, id)) {
      rc = rkl_push(&txn->wr.gc.reclaimed, id);
      if (unlikely(rc != MDBX_SUCCESS))
        break;
      rc = pnl_append_pnl(&txn->wr.repnl, glr.pnl);
      if (unlikely(rc != MDBX_SUCCESS))
        break;
    }

    if (!defrag_should_continue(dfc, pnl_size(glr.pnl))) {
      rc = MDBX_RESULT_TRUE;
      break;
    }

    rc = outer_next(gc, &gc_key, &gc_data, MDBX_NEXT);
  }

  if (rc == MDBX_NOTFOUND)
    rc = MDBX_SUCCESS;

  if (likely(!MDBX_IS_ERROR(rc))) {
    pnl_sort_nochk(txn->wr.repnl);
    if (unlikely(!pnl_check(txn->wr.repnl, txn->geo.first_unallocated)))
      return MDBX_PROBLEM;
  }

  return rc;
}

static int defrag_clear_reclaimed(dfc_t *dfc) {
  int rc = MDBX_SUCCESS;
  MDBX_txn *const txn = dfc->txn;
  if (!rkl_empty(&txn->wr.gc.reclaimed)) {
    MDBX_cursor *const gc = gc_cursor_init(txn);
    if (txn->dbs[FREE_DBI].items == rkl_len(&txn->wr.gc.reclaimed)) {
      rc = tbl_purge(gc);
      if (likely(rc == MDBX_SUCCESS)) {
        defrag_progress(dfc, /* TODO? */ rkl_len(&txn->wr.gc.reclaimed));
        rc = rkl_merge(&txn->wr.gc.reclaimed, &txn->wr.gc.ready4reuse, false);
        rkl_clear(&txn->wr.gc.reclaimed);
      }
    } else if (gc_may_clean_reclaimed(txn)) {
      gc->next = txn->cursors[FREE_DBI];
      txn->cursors[FREE_DBI] = gc;
      txn_refund(txn);
      do {
        MDBX_val gc_key;
        rc = outer_first(gc, &gc_key, nullptr);
        if (unlikely(rc != MDBX_SUCCESS))
          break;
        if (unlikely(gc_check_keylen(gc_key.iov_len))) {
          ERROR("%s/%d: %s", "corrupted GC-record", rc = MDBX_CORRUPTED, gc_check_keylen(gc_key.iov_len));
          break;
        }

        const txnid_t gc_first = unaligned_peek_u64(4, gc_key.iov_base);
        const txnid_t reclaimed_lower = rkl_pop(&txn->wr.gc.reclaimed, false);
        if (gc_first != reclaimed_lower) {
          ERROR("unexpected first-gc %" PRIaTXN " != reclaimed-lower %" PRIaTXN, gc_first, reclaimed_lower);
          rc = MDBX_PROBLEM;
          break;
        }

        if (txn->wr.loose_count > 0) {
          rc = gc_merge_loose(txn);
          if (unlikely(rc != MDBX_SUCCESS))
            break;
          txn_refund(txn);
        }
        rc = cursor_del(gc, 0);
        if (unlikely(rc != MDBX_SUCCESS))
          break;
        rc = rkl_push(&txn->wr.gc.ready4reuse, gc_first);
        if (unlikely(rc != MDBX_SUCCESS))
          break;
        txn_refund(txn);

        defrag_progress(dfc, /* TODO? */ 1);
        if (defrag_aborted(dfc)) {
          rc = MDBX_RESULT_TRUE;
          break;
        }
      } while (!rkl_empty(&txn->wr.gc.reclaimed) && gc_may_clean_reclaimed(txn));
      txn->cursors[FREE_DBI] = gc->next;
    }
  }

  if (likely(rc == MDBX_SUCCESS)) {
    if (txn->wr.loose_count > 0)
      rc = gc_merge_loose(txn);
    if (likely(rc == MDBX_SUCCESS))
      txn_refund(txn);
  }

  return rc;
}

static int defrag_puch_arc(dfc_t *const dfc, pgno_t parent, pgno_t page, unsigned npages, bool is_gc) {
  ASSERT(parent < MAX_PAGENO && page < MAX_PAGENO && (parent == 0 || parent >= NUM_METAS) && page >= NUM_METAS);
  dfc->payload_items += !is_gc;
  da_t *arc = dml_append(&dfc->arcs, page);
  if (likely(arc)) {
    arc->parent = parent;
    arc->npages = npages;
    arc->gc = !!is_gc;
    return MDBX_SUCCESS;
  } else
    return MDBX_ENOMEM;
}

static int defrag_walker(const size_t pgno, unsigned npages, void *const ctx, const unsigned deep,
                         const walk_tbl_t *table, const size_t page_size, const page_type_t page_type,
                         const txnid_t page_txnid, MDBX_error_t err, const size_t nentries, const size_t payload_bytes,
                         const size_t header_bytes, const size_t unused_bytes, const size_t parent_pgno) {
  (void)page_size;
  (void)page_txnid;
  (void)nentries;
  (void)payload_bytes;
  (void)header_bytes;
  (void)unused_bytes;
  (void)parent_pgno;

  dfc_t *const dfc = ctx;
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  if (unlikely((page_type & (P_BRANCH | P_LEAF | P_LARGE | P_DUPFIX | P_SUBP)) >= P_SUBP))
    return MDBX_SUCCESS;
  if (unlikely(deep >= ARRAY_LENGTH(dfc->walk_stack)))
    return MDBX_CORRUPTED;

/* используем старший бит для пометки уже отложенных страниц */
#define DEFRAG_TRACK_FLAG UINT32_C(0x80000000)
  STATIC_ASSERT(DEFRAG_TRACK_FLAG > MAX_PAGENO);

  ASSERT(deep == 0 || (dfc->walk_stack[deep - 1] & ~DEFRAG_TRACK_FLAG) == parent_pgno);
  ASSERT(pgno < DEFRAG_TRACK_FLAG);
  dfc->walk_stack[deep] = pgno;

  if (pgno >= dfc->walk_cutoff) {
    if (npages > 1 && pgno >= dfc->defrag_enough) {
      dfc->largepage_count += 1;
      dfc->largepage_amountleft += npages;
      if (npages > dfc->largepage_max)
        dfc->largepage_max = npages;
    }
    for (intptr_t i = deep; i >= 0 && dfc->walk_stack[i] < DEFRAG_TRACK_FLAG; --i) {
      err = defrag_puch_arc(dfc, i ? dfc->walk_stack[i - 1] & ~DEFRAG_TRACK_FLAG : 0, dfc->walk_stack[i],
                            (i == (intptr_t)deep) ? npages : 1, table->internal == &dfc->txn->dbs[FREE_DBI]);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      dfc->walk_stack[i] += DEFRAG_TRACK_FLAG;
    }
  }

  return defrag_should_continue(dfc, npages) ? MDBX_SUCCESS : MDBX_RESULT_TRUE;
}

__cold static int defrag_gc_lookup_page(dfc_t *dfc, pgno_t pgno, txnid_t *id) {
  *id = INVALID_TXNID;

  MDBX_val gc_key, gc_data;
  MDBX_txn *const txn = dfc->txn;
  MDBX_cursor *const gc = gc_cursor_init(txn);
  int rc = outer_first(gc, &gc_key, &gc_data);
  while (rc == MDBX_SUCCESS) {
    if (unlikely(gc_check_keylen(gc_key.iov_len))) {
      ERROR("%s/%d: %s", "corrupted GC-record", rc = MDBX_CORRUPTED, gc_check_keylen(gc_key.iov_len));
      break;
    }
    const glr_t glr = gc_row_pnl(txn, gc_data);
    if (unlikely(glr.err != MDBX_SUCCESS)) {
      ERROR("%s/%d: %s", "corrupted GC-record", rc = glr.err, glr.reason);
      break;
    }

    if (pnl_contains(glr.pnl, pgno)) {
      *id = unaligned_peek_u64(4, gc_key.iov_base);
      return MDBX_SUCCESS;
    }
    rc = outer_next(gc, &gc_key, &gc_data, MDBX_NEXT);
  }

  return rc;
}

MDBX_MAYBE_UNUSED static pgr_t defrag_get_page(dfc_t *dfc, pgno_t pgno) {
  MDBX_txn *const txn = dfc->txn;
  ASSERT(pgno >= NUM_METAS && pgno < txn->geo.first_unallocated);
  pgr_t pgr = page_get_unchecked(txn, pgno, txn_basis_snapshot(txn));
  if (likely(pgr.err == MDBX_SUCCESS) && unlikely(pgr.page->flags & ~(P_BRANCH | P_LEAF | P_DUPFIX | P_LARGE)))
    pgr.err = bad_page(pgr.page, "unexpected page flags 0x%x", pgr.page->flags);
  return pgr;
}

static int defrag_fixup_ref(dfc_t *dfc, void *pgno_ptr) {
  const pgno_t pgno = peek_pgno(pgno_ptr);
  if (pgno < NUM_METAS || pgno == P_INVALID)
    return MDBX_RESULT_FALSE;

  da_t *arc = dml_search_exact(dfc->arcs, pgno);
  if (arc) {
    if (arc->mapped) {
      poke_pgno(pgno_ptr, arc->mapped);
      return MDBX_RESULT_TRUE;
    }
    return MDBX_RESULT_FALSE;
  }
  if (pgno < dfc->walk_cutoff)
    return MDBX_RESULT_FALSE;
  ERROR("unexpected page %" PRIaPGNO " not found in the tracking map", pgno);
  return MDBX_PROBLEM;
}

static int defrag_fixup_page(dfc_t *dfc, page_t *dst, pgno_t pgno) {
  switch (dst->flags) {
  default:
    return bad_page(dst, "unexpected page flags 0x%x", dst->flags);
  case P_BRANCH:
    /* корректируем ссылки на дочерние страницы */
    for (size_t i = 0; i < page_numkeys(dst); ++i) {
      node_t *node = page_node(dst, i);
      int err;
      switch (node->flags) {
      default:
        return bad_page(dst, "unexpected node[%zu] flags 0x%x", i, node->flags);
      case 0 /* usual branch node */:
        err = defrag_fixup_ref(dfc, ptr_disp(node, offsetof(node_t, child_pgno)));
        if (unlikely(MDBX_IS_ERROR(err)))
          return err;
      }
    }
    break;
  case P_LEAF:
    /* корректируем ссылки на дочерние страницы */
    for (size_t i = 0; i < page_numkeys(dst); ++i) {
      node_t *node = page_node(dst, i);
      int err;
      switch (node->flags) {
      default:
        return bad_page(dst, "unexpected node[%zu] flags 0x%x", i, node->flags);
      case N_BIG /* large/overlow page */:
        err = defrag_fixup_ref(dfc, node_data(node));
        if (unlikely(MDBX_IS_ERROR(err)))
          return err;
        break;
      case N_TREE /* b-tree of named table */:
      case N_TREE | N_DUP /* nested b-tree of multi-values hive */:
        err = defrag_fixup_ref(dfc, ptr_disp(node_data(node), offsetof(tree_t, root)));
        if (unlikely(MDBX_IS_ERROR(err)))
          return err;
        if (err == MDBX_RESULT_TRUE)
          unaligned_poke_u64(1, ptr_disp(node_data(node), offsetof(tree_t, mod_txnid)), dfc->txn->txnid);
        break;
      case 0 /* usual leaf key-value node */:
      case N_DUP /* short sub-page */:
        break;
      }
    }
    break;
  case P_LEAF | P_DUPFIX:
  case P_LARGE:
    break;
  }

  dst->pgno = pgno;
  dst->txnid = dfc->txn->txnid;
  return MDBX_SUCCESS;
}

static int defrag_move(dfc_t *dfc, da_t *arc) {
  MDBX_txn *const txn = dfc->txn;
  ASSERT(arc->key_or_pgno >= NUM_METAS && arc->mapped >= NUM_METAS && arc->npages > 0);
  ASSERT(arc->key_or_pgno + arc->npages <= txn->geo.first_unallocated);
  ASSERT((size_t)arc->mapped + (size_t)arc->npages <= dfc->defrag_edge);
  int err;

  /* Нужно скопировать содержимое страницы в новое место, а также поправить ссылку на перемещённую страницу в
   * родительской. Родительская страница тоже должна быть скопирована/перемещена, вместе с её родительской и так до
   * корня дерева. Корректируемые ссылки точно отсутствуют только у небольшой части страниц, в large/overlow страницах и
   * листьях вложенных b-tree у dupsort-таблиц.
   *
   * Поэтому нерационально корректировать ссылки в родительских страницах при перемещении/копировании дочерних, так как
   * это потребует многократного обновления большей части родительских страниц. Напротив, корректировка ссылок внутри
   * страницы при её перемещении/копировании избавляет как от многократного обновления родительских страниц, так и от
   * сканирования в поисках подлежащих корректировке ссылок. Количество поисков в dfc->track при этом не меняется, так
   * как в обоих случаях равно количеству дочерних страниц. Кроме этого, последовательная обработка dfc->track приведет
   * к более упоряченному чтению и записи страниц. */

  const size_t npages = arc->npages;
  const size_t di = ((txn->flags & MDBX_WRITEMAP) == 0 || MDBX_AVOID_MSYNC) ? txn_dpl_exist(txn, arc->key_or_pgno) : 0;
  if (di) {
    page_t *dst = txn->wr.dirtylist->items[di].ptr;
    if (MDBX_AVOID_MSYNC && (txn->flags & MDBX_WRITEMAP)) {
      const page_t *const src = dst;
      dst = pgno2page(txn->env, arc->mapped);
      page_copy(dst, src, pgno2bytes(txn->env, npages));
    }

    /* Seems this is too rare case to make an optimizing dpl_move(txn, from_di, to_pgno) function. */
    txn_dpl_remove_ex(txn, di, npages);
    err = txn_dpl_append(txn, arc->mapped, dst, npages);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    dfc->cycle_pages_moved += npages;
    return defrag_fixup_page(dfc, dst, arc->mapped);
  }

  page_t *const dst =
      ((txn->flags & MDBX_WRITEMAP) && !MDBX_AVOID_MSYNC && env_is_page_incore(dfc->txn->env, arc->mapped))
          ? pgno2page(txn->env, arc->mapped)
          : txn->env->page_auxbuf;

  if (env_is_page_incore(dfc->txn->env, arc->key_or_pgno)) {
    const pgr_t pgr = defrag_get_page(dfc, arc->key_or_pgno);
    if (unlikely(pgr.err != MDBX_SUCCESS))
      return pgr.err;
    page_copy(dst, pgr.page, txn->env->ps);
  } else {
#if MDBX_CHECKING > 1
    ASSERT(!pnl_contains(dfc->repnl_clone, arc->key_or_pgno));
#endif /* MDBX_CHECKING > 1 */
    err = osal_pread(txn->env->dxb_mmap.fd, dst, txn->env->ps, pgno2bytes(txn->env, arc->key_or_pgno));
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }

  err = defrag_fixup_page(dfc, dst, arc->mapped);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  if (dst == txn->env->page_auxbuf) {
#if MDBX_CHECKING > 1
    ASSERT(pnl_contains(dfc->repnl_clone, arc->mapped));
#endif /* MDBX_CHECKING > 1 */
    err = osal_pwrite(txn->env->dxb_mmap.fd, dst, txn->env->ps, pgno2bytes(txn->env, arc->mapped));
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }

  DEBUG("moved %u pages %u -> %u", arc->npages, arc->key_or_pgno, arc->mapped);
  dfc->cycle_pages_moved += npages;
  if (unlikely(npages > 1)) {
    MDBX_env *env = txn->env;
    for (pgno_t i = 1; i < npages; ++i) {
      const size_t off_src = pgno2bytes(env, arc->key_or_pgno + i);
      const size_t off_dst = pgno2bytes(env, arc->mapped + i);
      if ((txn->flags & MDBX_WRITEMAP) && !MDBX_AVOID_MSYNC && env_is_page_incore(env, arc->mapped + i)) {
        if (env_is_page_incore(env, arc->key_or_pgno + i))
          memcpy(ptr_disp(env->dxb_mmap.base, off_dst), ptr_disp(env->dxb_mmap.base, off_src), env->ps);
        else {
          err = osal_pread(env->dxb_mmap.fd, ptr_disp(env->dxb_mmap.base, off_dst), env->ps, off_src);
          if (unlikely(err != MDBX_SUCCESS))
            return err;
        }
      } else {
#if MDBX_USE_COPYFILERANGE
        const ssize_t remainted = pgno2bytes(env, npages - i);
        off_t off_dst_proxy = off_dst;
        off_t off_src_proxy = off_src;
        const ssize_t copied =
            copy_file_range(env->dxb_mmap.fd, &off_src_proxy, env->dxb_mmap.fd, &off_dst_proxy, remainted, 0);
        err = MDBX_SUCCESS;
        if (unlikely(copied != remainted))
          err = (copied < 0) ? errno : MDBX_EIO;
        break;
#else
#if MDBX_CHECKING > 1
        ASSERT(!pnl_contains(dfc->repnl_clone, bytes2pgno(env, off_src)));
#endif /* MDBX_CHECKING > 1 */
        err = osal_pread(env->dxb_mmap.fd, txn->env->page_auxbuf, env->ps, off_src);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
#if MDBX_CHECKING > 1
        ASSERT(pnl_contains(dfc->repnl_clone, bytes2pgno(env, off_dst)));
#endif /* MDBX_CHECKING > 1 */
        err = osal_pwrite(txn->env->dxb_mmap.fd, txn->env->page_auxbuf, env->ps, off_dst);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
#endif /* MDBX_USE_COPYFILERANGE */
      }
    }
  }

  return MDBX_SUCCESS;
}

static pgno_t defrag_repnl_get(dfc_t *dfc, size_t npages) {
  if (likely(pnl_size(dfc->txn->wr.repnl) >= npages &&
             dfc->defrag_enough >= MDBX_PNL_LEAST(dfc->txn->wr.repnl) + npages)) {
    pgno_t pgno = dfc->largepage_amountleft ? pnl_get_best_sequence(dfc->txn->wr.repnl, npages, dfc->defrag_enough)
                                            : gc_repnl_get_single(dfc->txn);
    ASSERT(pgno == 0 || pgno + npages <= dfc->defrag_enough);
    return pgno;
  }
  return 0;
}

static int defrag_schedule(dfc_t *dfc, da_t *arc, pgno_t assigned) {
  da_t *stack_of_parents[CURSOR_STACK_SIZE * 2];
  size_t depth = 0;
  const size_t npages = arc->npages;

  ASSERT(dfc->remapped_edge < dfc->defrag_enough);
  for (da_t *chain = arc; chain->parent;) {
    chain = dml_search_exact(dfc->arcs, chain->parent);
    ASSERT(chain && chain->npages == 1);
    if (chain->mapped)
      break;

    chain->mapped = defrag_repnl_get(dfc, 1);
    VERBOSE("map%s %u pages %u/%u => %u", "-parent", 1, chain->parent, chain->key_or_pgno, chain->mapped);
    if (!chain->mapped)
      goto bailout;

    ASSERT((size_t)arc->mapped + 1 <= dfc->defrag_enough);
    stack_of_parents[depth++] = chain;
  }

  ASSERT(!arc->mapped);
  if (!assigned)
    assigned = defrag_repnl_get(dfc, npages);
  arc->mapped = assigned;
  VERBOSE("map%s %zu pages %u/%u => %u", "", npages, arc->parent, arc->key_or_pgno, arc->mapped);

  if (arc->mapped) {
    ASSERT(arc->mapped + npages <= dfc->retreat_edge);
    if (npages > 1 && arc->key_or_pgno >= dfc->defrag_enough) {
      ASSERT(dfc->largepage_amountleft >= npages);
      dfc->largepage_amountleft -= npages;
    }
    if (dfc->remapped_edge < arc->mapped)
      dfc->remapped_edge = arc->mapped + npages - 1;
    for (size_t i = 0; i < depth; ++i) {
      da_t *chain = stack_of_parents[i];
      if (dfc->remapped_edge < chain->mapped)
        dfc->remapped_edge = chain->mapped;
    }
    if (likely(dfc->remapped_edge < dfc->retreat_edge)) {
      dfc->cycle_pages_scheduled += npages + depth;
      DEBUG("scheduled %zu+%zu=%zu pages %u -> %u", npages, depth, npages + depth, arc->key_or_pgno, arc->mapped);
      return MDBX_SUCCESS;
    }
  }

bailout:
  VERBOSE("unable find/assign remapping for %zu-pages span, chain-depth %zu, repnl-len %zu, repnl-least %u, detent %u",
          npages, depth, pnl_size(dfc->txn->wr.repnl),
          pnl_size(dfc->txn->wr.repnl) ? MDBX_PNL_LEAST(dfc->txn->wr.repnl) : P_INVALID, dfc->defrag_enough);
  if (pnl_size(dfc->txn->wr.repnl) == 0)
    defrag_stumble(dfc, arc, "are not enough reclaimable pages available in GC", 0, "");
  else if (MDBX_PNL_LEAST(dfc->txn->wr.repnl) + npages >= dfc->defrag_enough)
    defrag_stumble(dfc, arc, "the next reclaimable page ", MDBX_PNL_LEAST(dfc->txn->wr.repnl),
                   " is beyond target edge");
  else {
    ASSERT(npages > 1);
    if ((dfc->stopping_reasons & MDBX_defrag_large_chunk) == 0) {
      dfc->stopping_reasons |= MDBX_defrag_large_chunk;
      defrag_stumble(dfc, arc, "is no span of ", npages, " consecutive/adjacent reclaimable pages");
    }
  }

  /* Не получилось, возвращаем всё выделенное для родительских страниц */
  while (depth > 0) {
    da_t *chain = stack_of_parents[--depth];
    pnl_append_prereserved(dfc->txn->wr.repnl, chain->mapped);
    chain->mapped = 0;
  }

  if (assigned) {
    int err = pnl_append_span(&dfc->txn->wr.repnl, assigned, npages);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }
  arc->mapped = 0;

  pnl_sort(dfc->txn->wr.repnl, dfc->txn->geo.first_unallocated);
  return MDBX_RESULT_TRUE;
}

__hot __noinline static unsigned defrag_move_cost_uncached(dfc_t *dfc, pgno_t pgno, pgno_t span) {
  if (pgno < NUM_METAS || pgno + span >= dfc->retreat_edge)
    return INT_MAX;

  da_t *arc = dml_search_exact(dfc->arcs, pgno);
  if (!arc)
    return pnl_contains(dfc->txn->wr.repnl, pgno) ? 0 : INT_MAX;
  if (arc->mapped)
    return arc->engaged ? INT_MAX : 0;

  size_t npages = arc->npages;
  size_t cost = npages;
  if (npages > 1) {
    if (npages >= span)
      return INT_MAX;
    cost += 64 << ((npages < 12) ? npages : 12);
  }
  while (arc->parent) {
    arc = dml_search_exact(dfc->arcs, arc->parent);
    ASSERT(arc != nullptr);
    if (arc->mapped)
      break;
    cost += arc->key_or_pgno < dfc->retreat_edge;
  }
  return cost;
}

static inline unsigned defrag_move_cost(dfc_t *dfc, pgno_t pgno, pgno_t span) {
  struct cache_item *cache = &dfc->cache_cost[pgno % ARRAY_LENGTH(dfc->cache_cost)];
  if (cache->pgno == pgno)
    return cache->cost;
  cache->pgno = pgno;
  return cache->cost = defrag_move_cost_uncached(dfc, pgno, span);
}

__hot static int defrag_provide_span(dfc_t *const dfc, const size_t npages) {
  ASSERT(npages > 1);
  const pnl_t pnl = dfc->txn->wr.repnl;
  const size_t len = pnl_size(pnl);

  if (len < npages)
    return MDBX_RESULT_TRUE;

  memset(dfc->cache_cost, 0, sizeof(dfc->cache_cost));
  size_t best_begin = MAX_PAGENO, best_cost = len;
#if MDBX_PNL_ASCENDING
#error "FIXME: Since 2026-04-01 alternatives to MDBX_PNL_ASCENDING = 0 are no longer supported."
#else
  for (size_t span, i = len; i > 0 && pnl[i] <= dfc->defrag_enough - npages;) {
    span = 1;
    while (unlikely(MDBX_PNL_CONTIGUOUS(pnl[i - span], pnl[i], span)) &&
           likely((intptr_t)(i - span) > 0 && pnl[i - span] < dfc->defrag_enough))
      ++span;

    pgno_t begin = pnl[i], end = begin + span;
    i -= span;
    size_t cost = 0;
    size_t cost_left = defrag_move_cost(dfc, begin - 1, npages);
    size_t cost_right = defrag_move_cost(dfc, end, npages);
    do {
      if (cost_left < cost_right) {
        cost += cost_left;
        cost_left = defrag_move_cost(dfc, --begin - 1, npages);
      } else if (cost_right < INT_MAX) {
        cost += cost_right;
        cost_right = defrag_move_cost(dfc, ++end, npages);
      } else {
        while (pnl[i] < end && likely(i > 0))
          --i;
        break;
      }
    } while (cost < best_cost && end - begin < npages);

    if (unlikely(end - begin == npages && cost < best_cost)) {
      best_cost = cost;
      best_begin = begin;
      if (!best_cost)
        break;
    }
  }
#endif /* MDBX_PNL_ASCENDING */

  if (/* paranoia */ best_begin + npages >= dfc->retreat_edge || best_begin + npages >= dfc->defrag_edge)
    return MDBX_RESULT_TRUE;

  /* Сначала убираем из repnl выбранные страницы, чтобы не использовать их на перемещение остальных. */
  pnl_clear(dfc->temp);
  pnl_cut_range(pnl, &dfc->temp, best_begin, best_begin + npages);

  /* Теперь сортируем вынутое из repnl и подготавливаем перемещение используемых страниц. */
  const size_t repnl_before = pnl_size(pnl);
  size_t payoff = 0;
  for (pgno_t pgno = best_begin + npages; --pgno >= best_begin;) {
    if (!pnl_contains(dfc->temp, pgno)) {
      da_t *arc = dml_search_exact(dfc->arcs, pgno);
      ASSERT(arc != nullptr);
      if (unlikely(!arc || arc->npages >= npages))
        return MDBX_PROBLEM;
      if (!arc->mapped) {
        int rc = defrag_schedule(dfc, arc, 0);
        if (rc != MDBX_SUCCESS) {
          int err = pnl_append_pnl(&dfc->txn->wr.repnl, dfc->temp);
          ASSERT(!err);
          if (/* paranoia */ unlikely(err != MDBX_SUCCESS))
            return err;
          pnl_sort(dfc->txn->wr.repnl, dfc->txn->geo.first_unallocated);
          return rc;
        }
      } else {
        ASSERT(!arc->engaged);
        payoff += 1;
      }
      arc->engaged = 1;
    }
  }

  int err = pnl_append_pnl(&dfc->lp_reserve, dfc->temp);
  ASSERT(!err);
  if (/* paranoia */ unlikely(err != MDBX_SUCCESS))
    return err;

  const size_t moves = repnl_before - pnl_size(pnl);
  VERBOSE("prepared lp-span %zu, at %zu, %zu moves, %zu repnl", npages, best_begin, moves, pnl_size(dfc->temp));
  ASSERT(pnl_size(dfc->temp) <= npages);
  ASSERT(moves + payoff + pnl_size(dfc->temp) >= npages);
  ASSERT(moves + payoff || npages == pnl_size(dfc->temp));
  (void)payoff;
  return MDBX_SUCCESS;
}

int defrag_cycle(dfc_t *dfc) {
  MDBX_txn *const txn = dfc->txn;
  if (dfc->cycle) {
    dfc->stopping_reasons &= ~(MDBX_defrag_large_chunk | MDBX_defrag_step_size);
    dfc->progress_counter = 0;
    dfc->cycle_pages_scheduled = 0;
    dfc->cycle_pages_moved = 0;
    dfc->cycle_preprogress = 0;
    dfc->cycle += 1;
  }
  if (!defrag_should_continue(dfc, 0))
    return MDBX_RESULT_TRUE;

  int rc = defrag_load_gc(dfc);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  defrag_milestone(dfc);
  rc = txn_basal_update_tbl_roots(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (txn->wr.loose_count > 0) {
    rc = gc_merge_loose(txn);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  dfc->cycle_initial_score = defrag_score(dfc, dfc->txn->geo.first_unallocated);
  VERBOSE("after-%s: %u => %u (%i), GC-score %" PRIu64, "GC-load", dfc->before_defrag, dfc->txn->geo.first_unallocated,
          dfc->before_defrag - dfc->txn->geo.first_unallocated, dfc->cycle_initial_score);

  /* Проверяем сумму использованных страниц */
  const size_t gc_pages = dfc->gc_tree_pages + dfc->gc_retained_pages;
  const size_t pending_pages =
      txn->wr.loose_count + pnl_size(txn->wr.repnl) + (pnl_size(txn->wr.retired_pages) - /* retired_stored */ 0);
  if (dfc->payload_pages + gc_pages + pending_pages != txn->geo.first_unallocated) {
    ERROR("page usage mismatch (payload %u + gc %zu + pending %zu != allocated %zu), "
          "please use mdbx_chk tool to check DB integrity",
          dfc->payload_pages, gc_pages, pending_pages, (size_t)txn->geo.first_unallocated);
    return LOG_IFERR(MDBX_PROBLEM);
  }

  if (likely(!MDBX_IS_ERROR(rc))) {
    int err = defrag_clear_reclaimed(dfc);
    if (unlikely(err != MDBX_SUCCESS))
      rc = err;
  }
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  defrag_milestone(dfc);
  uint64_t score = defrag_score(dfc, dfc->txn->geo.first_unallocated);
  VERBOSE("after-%s: %u => %u (%i), GC-score %" PRIu64, "GC-clean", dfc->before_defrag, dfc->txn->geo.first_unallocated,
          dfc->before_defrag - dfc->txn->geo.first_unallocated, score);
  if (dfc->payload_pages == 0 || !pnl_size(txn->wr.repnl))
    return defrag_gc_empty(dfc) ? MDBX_RESULT_TRUE : MDBX_LAGGARD_READER;

  if (!dfc->lp_backlog && (txn->dbi_state[MAIN_DBI] & DBI_DIRTY) == 0 && score < dfc->cycle_initial_score) {
    NOTICE("bailout since the GC-score after load and clear 0x%" PRIx64 " is wort than before 0x%" PRIx64, score,
           dfc->cycle_initial_score);
    return MDBX_RESULT_TRUE;
  }

#if MDBX_CHECKING > 1
  dfc->repnl_clone = pnl_clone(txn->wr.repnl);
#endif /* MDBX_CHECKING > 0 */

  if (dfc->payload_items) {
    /* Повторный проход. Все страницы с полезными данными по-прежнему используются, но были перемещены. Отличия могут
     * быть только в страницах связанных с GC. Поэтому корректируем карту по результатам перемещения страниц и удаляем
     * элементы связанные с GC, которые затем будут загружены повторно. */
    da_t *const begin = dfc->arcs->items, *const end = begin + dfc->arcs->length;
    for (da_t *i = begin; i != end; ++i) {
      /* Первым проходом корректируем ссылки на родительские страницы по результатам перемещения, иначе после изменения
       * основных номеров связь будет потеряна. */
      if (likely(i->parent)) {
        da_t *p = dml_search_exact(dfc->arcs, i->parent);
        ASSERT(p != nullptr && p->key_or_pgno == i->parent);
        i->parent = p->mapped ? p->mapped : p->key_or_pgno;
      }
    }

    dfc->remapped_edge = 0;
    dfc->largepage_count = 0;
    dfc->largepage_amountleft = 0;
    dfc->largepage_max = 0;
    da_t *w = begin;
    for (da_t *r = begin; r != end; ++r) {
      /* Корректируем номера страниц по результатам перемещения и удаляем/пропускаем элементы связанные с GC. */
      if (!r->gc) {
        if (r->mapped) {
          if (dfc->stumble_pgno == r->key_or_pgno) {
            dfc->stumble_pgno = 0;
            dfc->stumble_span = 0;
          }
          if (dfc->lp_backlog == r->key_or_pgno)
            dfc->lp_backlog = 0;
          r->key_or_pgno = r->mapped;
          r->mapped = 0;
          r->engaged = 0;
        }
        *w = *r;
        const size_t npages = w->npages;
        if (npages > 1 && w->key_or_pgno >= dfc->defrag_enough) {
          dfc->largepage_count += 1;
          dfc->largepage_amountleft += npages;
          if (npages > dfc->largepage_max)
            dfc->largepage_max = npages;
        }
        ++w;
      }
    }
    dfc->arcs->length = w - begin;
    if (unlikely(dfc->arcs->length != dfc->payload_items)) {
      ERROR("mismatch entries (%zu != expected %zu) after filter-out GC on next pass", dfc->arcs->length,
            dfc->payload_items);
      return MDBX_PROBLEM;
    }
  } else {
    env_clear_incore_cache(txn->env);
  }

  /* Обходим b-tree и сохраняем в dfc->arcs[] номера страниц больше dfc->walk_cutoff, вместе с их родительскими
   * страницами на всем пути от корня b-tree. */
  rc = walk_pages(txn, defrag_walker, dfc, dfc->payload_items ? dont_walk_MAIN : dont_check_keys_ordering);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
  if (dfc->arcs->length < 1)
    return MDBX_SUCCESS;

  if (!dfc->cycle) {
    defrag_milestone(dfc);
    dfc->progress_counter = 0;
    dfc->cycle = 1;
  }
  dfc->cycle_preprogress = dfc->progress_counter;
  defrag_milestone(dfc);
  ASSERT(dfc->cycle_pages_scheduled == 0 && dfc->cycle_pages_moved == 0);

  /* После сортировки в начале массива dfc->arcs[] располагается некоторое количество пар целевая-родительская
   * номеров страниц, из которых необходимо переместить данные. Перемещать содержимое страниц есть смысл пока
   * выполняются два условия:
   *  - очередная исходная страница с наибольшим номером прилегает к концу БД (к границе распределенных страниц);
   *  - очередная целевая переработанная страница ближе к началу БД чем исходная.
   *
   * Однако, некоторые сложности возникают из-за large/overflow-страниц, для которых необходимо выделять/искать
   * последовательности свободных страниц. Соответственно, такие последовательности требуется экономить (не расходовать
   * на одиночные страницы или на интервалы меньшего размера). Поэтому при наличии хотя-бы одной large/overflow-страницы
   * размером более одной страницы выделение выполняется с поиском наиболее подходящей по размеру последовательности,
   * включая одиночные.
   *
   * Далее, перемещение длинных large/overflow-страниц может потребовать предварительного формирования соответствующих
   * последовательностей свободных страниц, т.е. предварительного перемещения других страниц внутри теля БД, которые не
   * требуют перемещения.
   *
   * Далее, перемещение данных потребует корректировки ссылок в родительских страницах по цепочки от листовых до корня.
   * Соответственно, нет смысла записывать какие-либо родительские страницы до завершения создания карты перемещений. */

  dml_sort(dfc->arcs);
  pnl_sort(txn->wr.retired_pages, txn->geo.first_unallocated);
  const pgno_t most_retired = pnl_size(txn->wr.retired_pages) ? MDBX_PNL_MOST(txn->wr.retired_pages) : 0;
  ASSERT(MDBX_PNL_MOST(txn->wr.repnl) < txn->geo.first_unallocated - 1);
  dfc->retreat_edge = dfc->defrag_enough;
  dfc->defrag_edge = txn->geo.first_unallocated;
  da_t *const begin = dfc->arcs->items, *const end = begin + dfc->arcs->length;

  if (dfc->largepage_count > 0) {
    /* Есть large/overflow-страницы длиной больше 1, для которых требует много дополнительных действий */
    if (!dfc->lp_reserve) {
      /* Резервирование выполняется один раз, на все последующие циклы */
      dfc->lp_reserve = pnl_alloc(dfc->largepage_amountleft);
      dfc->temp = pnl_alloc(dfc->largepage_max);
      if (!dfc->lp_reserve || !dfc->temp)
        return MDBX_ENOMEM;
    }
  }

  da_t *lp = nullptr;
  if (dfc->lp_backlog) {
    lp = dml_search_exact(dfc->arcs, dfc->lp_backlog);
    ASSERT(lp != nullptr);
    if (lp) {
      ASSERT(lp->npages > 1);
      pgno_t pgno = pnl_get_best_sequence(dfc->txn->wr.repnl, lp->npages, dfc->defrag_enough);
      if (pgno) {
        int err = defrag_schedule(dfc, lp, pgno);
        if (err == MDBX_SUCCESS)
          lp = nullptr;
        else if (err != MDBX_RESULT_TRUE)
          return err;
      }
    }
  }

  for (da_t *i = begin; !defrag_discontinued(dfc) && i != end && i->key_or_pgno >= dfc->retreat_edge && !lp; ++i) {
    if (i->key_or_pgno < dfc->remapped_edge) {
      defrag_stumble(dfc, i,
                     "all the unused pages suitable for defragmentation have been used up, the re-mapped edge is ",
                     dfc->remapped_edge, "");
      break;
    }

    if (dfc->defrag_edge - i->npages != i->key_or_pgno) {
      const pgno_t tail = dfc->defrag_edge - 1;
      if (most_retired == tail) {
        defrag_stumble(dfc, i, "the adjacent page ", most_retired, " is in the retired list");
        break;
      }
      if (pnl_size(txn->wr.repnl) && MDBX_PNL_LEAST(txn->wr.repnl) == tail) {
        defrag_stumble(dfc, i, "the available reclaimed pages are beyond the defragmentation tail", 0, "");
        break;
      }
      int err = defrag_gc_lookup_page(dfc, dfc->defrag_edge - 1, &dfc->stopor);
      if (unlikely(err != MDBX_SUCCESS)) {
        ERROR("unexpected missing page %" PRIaPGNO, tail);
        return (err == MDBX_NOTFOUND) ? MDBX_PROBLEM : err;
      }
      defrag_stumble(dfc, i, "the gap-page is located in the ", dfc->stopor, " GC-entry which is used/read");
      break;
    }

    if (!i->mapped) {
      int err = defrag_schedule(dfc, i, 0);
      if (unlikely(err != MDBX_SUCCESS)) {
        ASSERT((i->mapped != 0) == (err == MDBX_SUCCESS));
        if (unlikely(err != MDBX_RESULT_TRUE))
          return err;
        lp = (i->npages > 1) ? i : nullptr;
        break;
      }
    }
    VERBOSE("turn-%s %+i, %u => %u", "move", i->npages, dfc->defrag_edge, i->key_or_pgno);
    dfc->defrag_edge = i->key_or_pgno;

    if (pnl_size(txn->wr.repnl) > 0 && MDBX_PNL_MOST(txn->wr.repnl) == dfc->defrag_edge - 1) {
      const pgno_t npages = pnl_crop_tail_sequence(txn->wr.repnl);
      rc = pnl_append_span(&txn->wr.retired_pages, dfc->defrag_edge - npages, npages);
      if (unlikely(rc != MDBX_SUCCESS))
        return rc;
      VERBOSE("turn-%s %+i, %u => %u", "crop", npages, dfc->defrag_edge, dfc->defrag_edge - npages);
      dfc->defrag_edge -= npages;
    }
    defrag_progress(dfc, i->npages);
    if (dfc->cycle_pages_scheduled >= dfc->move_batch_size) {
      dfc->stopping_reasons |= MDBX_defrag_step_size;
      break;
    }
  }

  if (lp || dfc->stopping_reasons == MDBX_defrag_large_chunk) {
    /* Для быстрой дефрагментации требуется использовать имеющиеся последовательности свободных страниц максимально
     * эффективно:
     *  - с одной стороны, нет смысла перемещать ближние куски раньше дальних, иначе дефрагментация всё равно
     *    остановится на не перемещённых дальних;
     *  - с другой стороны, выделяя последовательности для коротких кусков, можно потерять возможность
     *    выделить и/или сформировать последовательности для последующих более длинных.
     *
     * По результатам серии экспериментов, лучше всех оказался такой подход:
     *  1. Планируем перемещение регулярных/одиночных страниц и large/overflow-страниц, двигаясь от конца БД к началу,
     *     пока удаётся выделять последовательности.
     *  2. Если останавливаемся из-за отсутствия свободной последовательности необходимой длины, то
     *     прерываем основное регулярный цикл перемещения и далее в текущем цикле дефрагментации занимается
     *     только large/overflow-страницами.
     *  3. Для каждой large/overflow-странице пытаемся найти наиболее подходящую последовательность свободных страниц,
     *     а при отсутствии подготавливаем интервал свободных страниц для перемещения на следующем цикле. */
    defrag_milestone(dfc);
    for (da_t *i = lp ? lp : begin;
         !defrag_discontinued(dfc) && i != end && dfc->largepage_amountleft &&
         pnl_size(dfc->txn->wr.repnl) > dfc->largepage_amountleft && i->key_or_pgno > dfc->remapped_edge &&
         i->key_or_pgno > MDBX_PNL_LEAST(dfc->txn->wr.repnl);
         defrag_milestone(dfc), ++i) {
      if (!i->mapped && i->npages > 1) {
        ASSERT(dfc->largepage_amountleft >= i->npages);
        if (i != lp) {
          pgno_t pgno = pnl_get_best_sequence(dfc->txn->wr.repnl, i->npages, dfc->defrag_enough);
          if (pgno) {
            int err = defrag_schedule(dfc, i, pgno);
            if (unlikely(err != MDBX_SUCCESS)) {
              if (err == MDBX_RESULT_TRUE)
                break;
              return err;
            }
          }
        }
        if (!i->mapped) {
          int err = defrag_provide_span(dfc, i->npages);
          if (err != MDBX_SUCCESS) {
            if (unlikely(err != MDBX_RESULT_TRUE))
              return err;
            break;
          }
          dfc->largepage_amountleft -= i->npages;
          dfc->lp_backlog = (dfc->lp_backlog < i->key_or_pgno) ? i->key_or_pgno : dfc->lp_backlog;
        }
        defrag_progress(dfc, i->npages);
        if (dfc->cycle_pages_scheduled >= dfc->move_batch_size) {
          dfc->stopping_reasons |= MDBX_defrag_step_size;
          break;
        }
      }
    }
  }

  score = defrag_score(dfc, dfc->defrag_edge);
  defrag_milestone(dfc);
  VERBOSE("after-%s: %u => %u (%i), GC-score %" PRIu64, "tree-sift", dfc->before_defrag, dfc->defrag_edge,
          dfc->before_defrag - dfc->defrag_edge, score);
  if (dfc->lp_reserve) {
    rc = pnl_append_pnl(&txn->wr.retired_pages, dfc->lp_reserve);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
    pnl_setsize(dfc->lp_reserve, 0);
  }
  if (/* paranoia */ txn->flags & MDBX_TXN_ERROR)
    return MDBX_EIO;
  for (da_t *i = begin; i != end; ++i) {
    if (i->mapped) {
      ASSERT(i->mapped < dfc->defrag_edge);
      rc = defrag_move(dfc, i);
      if (unlikely(rc != MDBX_SUCCESS))
        break;
      rc = pnl_append_span(&txn->wr.retired_pages, i->key_or_pgno, i->npages);
      if (unlikely(rc != MDBX_SUCCESS))
        break;
    }
    defrag_progress(dfc, i->npages);
    if (defrag_aborted(dfc)) {
      rc = MDBX_RESULT_TRUE;
      break;
    }
  }

  defrag_milestone(dfc);
  if (rc != MDBX_SUCCESS) {
    txn->flags |= MDBX_TXN_ERROR;
    return rc;
  }

  ASSERT(dfc->cycle_pages_scheduled == dfc->cycle_pages_moved);
  if (dfc->cycle_pages_moved) {
    dfc->total_pages_moved += dfc->cycle_pages_moved;
    TXN_FOREACH_DBI_ALL(txn, dbi) {
      if ((txn->dbi_state[dbi] & (DBI_VALID | DBI_LINDO | DBI_STALE)) == (DBI_VALID | DBI_LINDO) &&
          txn->dbs[dbi].root != P_INVALID) {
        da_t *dm = dml_search_exact(dfc->arcs, txn->dbs[dbi].root);
        if (dm && dm->mapped != 0) {
          txn->dbs[dbi].root = dm->mapped;
          txn->dbi_state[dbi] |= DBI_DIRTY;
          txn->flags |= MDBX_TXN_DIRTY;
        }
      }
    }
  } else {
    ASSERT(dfc->defrag_edge == txn->geo.first_unallocated);
    ASSERT(dfc->cycle_pages_scheduled == 0);
  }

  score = defrag_score(dfc, dfc->txn->geo.first_unallocated);
  defrag_milestone(dfc);
  VERBOSE("after-%s: %u => %u (%i), GC-score %" PRIu64, "move-pages", dfc->before_defrag, txn->geo.first_unallocated,
          dfc->before_defrag - txn->geo.first_unallocated, score);

  return (dfc->stumble_pgno || (txn->dbi_state[MAIN_DBI] & DBI_DIRTY) || score > dfc->cycle_initial_score)
             ? MDBX_SUCCESS
             : MDBX_RESULT_TRUE;
}

void defrag_destroy(dfc_t *dfc) {
  dml_free(&dfc->arcs);
  pnl_free(dfc->temp);
  dfc->temp = nullptr;
  pnl_free(dfc->lp_reserve);
  dfc->lp_reserve = nullptr;
}

int defrag_init(dfc_t *dfc, MDBX_txn *txn, size_t defrag_atleast_pages, size_t spend_atleast_wallclock_dot16,
                size_t defrag_enough_pages, size_t limit_spend_wallclock_dot16, intptr_t preferred_move_batch_size) {
  memset(dfc, 0, sizeof(*dfc));
  dfc->start_timestamp = osal_monotime();
  if (preferred_move_batch_size < 0) {
    preferred_move_batch_size = 0;
    if (limit_spend_wallclock_dot16) {
      /* TODO: подстроить размер (возможно динамически на каждом цикле) так,
               чтобы время записи порции не превышало 5-10% от таймаута. */
      preferred_move_batch_size = bytes2pgno(txn->env, 64 * MEGABYTE);
    }
  }
  dfc->move_batch_size = (preferred_move_batch_size && (size_t)preferred_move_batch_size < MAX_PAGENO)
                             ? (size_t)preferred_move_batch_size
                             : MAX_PAGENO;

  dfc->txn = txn;
  dfc->before_defrag = txn->geo.first_unallocated;
  dfc->last_allocated = dfc->before_defrag;
  if (limit_spend_wallclock_dot16)
    dfc->wallclock_detent = dfc->start_timestamp + osal_16dot16_to_monotime(limit_spend_wallclock_dot16);
  if (spend_atleast_wallclock_dot16)
    dfc->wallclock_atleast = dfc->start_timestamp + osal_16dot16_to_monotime(spend_atleast_wallclock_dot16);

  /* Загружаем информацию о всех таблицах. */
  MDBX_stat stat;
  int err = tbl_stat_summary(txn, &stat);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  dfc->payload_pages =
      NUM_METAS + (size_t)stat.ms_branch_pages + (size_t)stat.ms_leaf_pages + (size_t)stat.ms_overflow_pages;
  dfc->largepage_max = /* устанавливаем в 1 как признак наличия overload/large-страниц */ stat.ms_overflow_pages > 0;
  dfc->summary_depth = stat.ms_depth;

  /* По количеству используемых и выделенных страниц, оцениваем сколько можем дефрагментировать/освободить. */
  const size_t max_defrag = txn->geo.first_unallocated - dfc->payload_pages;
  if (txn->geo.first_unallocated < dfc->payload_pages)
    return MDBX_PROBLEM;
  if (unlikely(max_defrag == 0))
    return MDBX_SUCCESS;

  err = pnl_reserve(&txn->wr.repnl, max_defrag);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  err = pnl_reserve(&txn->wr.retired_pages, max_defrag);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  if (txn->dbs[FREE_DBI].items) {
    err = rkl_reserve(&txn->wr.gc.reclaimed, txn->dbs[FREE_DBI].items);
    if (unlikely(err != MDBX_SUCCESS))
      return err;
  }

  dfc->defrag_atleast = txn->geo.first_unallocated;
  if (defrag_atleast_pages)
    dfc->defrag_atleast =
        (defrag_atleast_pages < max_defrag) ? txn->geo.first_unallocated - defrag_atleast_pages : dfc->payload_pages;
  dfc->defrag_enough = dfc->payload_pages;
  if (defrag_enough_pages && defrag_enough_pages < max_defrag)
    dfc->defrag_enough = txn->geo.first_unallocated - defrag_enough_pages;

  /* Необходимо найти в структуре дерева страницы расположенные близко к концу файла БД, а затем переместить их
   * содержимое в страницы ближе к началу БД. При этом для каждой дочерней страницы придеться делать также копию её
   * родительской. Поэтому, при поиске страниц, необходимо запоминать всю цепочку страниц, от корня до каждой целевой.
   * Удобнее всего для этого использовать граф в форме набора пар/дуг из номеров страниц дочерняя-родительская. */
  size_t arcs_needed;
  if (!dfc->largepage_max) {
    /* Если нет large/overflow страниц, то необходимо отследить только подлежащие перемещению и их родительские
     * страницы в структуре дерева. Количество перемещаемых страниц точно известно, а количество родительских связей
     * можно оценить/ограничить сверху через общую высоту деревьев и количество страниц. */
    size_t extra_for_parents = (size_t)(((uint64_t)dfc->summary_depth) * max_defrag / dfc->payload_pages);
    arcs_needed = max_defrag + extra_for_parents;
    if (arcs_needed > dfc->payload_pages)
      arcs_needed = dfc->payload_pages;
    dfc->walk_cutoff = dfc->payload_pages;
  } else {
    /* Если есть large/overflow страницы, то для их перемещения потребуются последовательности смежных свободных
     * страниц. В свою очередь, может потребоваться перемещение страниц внутри основного тела БД для формирования
     * таких последовательностей для чего потребуется информация о всех страницах с полезными данными. */
    arcs_needed = dfc->payload_pages;
    dfc->walk_cutoff = NUM_METAS;
  }
  dfc->arcs = dml_alloc(arcs_needed);
  if (unlikely(!dfc->arcs))
    return MDBX_ENOMEM;

  const size_t lo = (dfc->payload_pages < max_defrag) ? dfc->payload_pages : max_defrag;
  const size_t hi = (dfc->payload_pages > max_defrag) ? dfc->payload_pages : max_defrag;
  dfc->notify_watchmask = 7;
  while (dfc->notify_watchmask < (hi >> 10) && dfc->notify_watchmask < (lo >> 7) && dfc->notify_watchmask < 500)
    dfc->notify_watchmask += dfc->notify_watchmask + 1;

  return defrag_should_continue(dfc, 0) ? MDBX_SUCCESS : MDBX_RESULT_TRUE;
}
