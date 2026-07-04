/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2025-2026

#include "internals.h"

__cold int mdbx_gc_info(MDBX_txn *txn, MDBX_gc_info_t *info, size_t bytes, MDBX_gc_iter_func iter_func,
                        void *iter_ctx) {
  if (unlikely(!info || bytes != sizeof(MDBX_gc_info_t)))
    return MDBX_EINVAL;
  memset(info, 0, sizeof(*info));

  int rc = check_txn(txn, MDBX_TXN_BLOCKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  info->pages_total = txn->geo.upper;
  info->pages_backed = txn->geo.end_pgno;
  info->pages_allocated = txn->geo.first_unallocated;

  const volatile lck_t *const lck = txn->env->lck;
  if (lck) {
    do {
      info->max_reader_lag = lck->pgops.gc_prof.max_reader_lag;
      info->max_retained_pages = lck->pgops.gc_prof.max_retained_pages;
      osal_memory_fence(mo_AcquireRelease, false);
    } while (unlikely(info->max_reader_lag != lck->pgops.gc_prof.max_reader_lag ||
                      info->max_retained_pages != lck->pgops.gc_prof.max_retained_pages));
  }

  const tree_t *const gc = &txn->dbs[FREE_DBI];
  info->pages_gc += gc->branch_pages;
  info->pages_gc += gc->leaf_pages;
  info->pages_gc += gc->large_pages;

  const txnid_t reclaiming_detent = (txn->flags & txn_ro_flat) ? mvcc_snapshot_oldest_ro(txn, false).oldest_txnid
                                                               : mvcc_snapshot_oldest_rw(txn).oldest_txnid;
  if ((txn->flags & txn_ro_flat) == 0) {
    const size_t workset = txn->wr.loose_count + pnl_size(txn->wr.repnl);
    info->gc_reclaimable.pages += workset;
    info->gc_reclaimable.pages += info->pages_gc;
    info->pages_gc += workset + pnl_size(txn->wr.retired_pages) - /* retired_stored */ 0;
  }

  cursor_couple_t cx;
  rc = cursor_init(&cx.outer, txn, FREE_DBI);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  MDBX_val gc_key, gc_data;
  rc = outer_first(&cx.outer, &gc_key, &gc_data);
  if (rc == MDBX_SUCCESS) {
    cx.outer.next = txn->cursors[FREE_DBI];
    txn->cursors[FREE_DBI] = &cx.outer;
    for (;;) {
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
      if ((txn->flags & txn_ro_flat) == 0 && gc_is_reclaimed(txn, id))
        continue;

      const size_t len = pnl_size(glr.pnl);
      const bool is_reclaimable = id <= reclaiming_detent;
      info->pages_gc += len;
      if (is_reclaimable)
        info->gc_reclaimable.pages += len;
      for (size_t span, i = 1; i <= len; i += span) {
        span = pnl_scan_span(glr.pnl, i);
        const size_t pgno = glr.pnl[i];
        if (is_reclaimable) {
          histogram_acc(span, &info->gc_reclaimable.span_histogram);
          histogram_acc(pgno, &info->gc_reclaimable.pgno_distribution);
        }
        if (iter_func) {
          rc = iter_func(iter_ctx, txn, id, pgno, span, is_reclaimable);
          if (rc != MDBX_RESULT_FALSE)
            break;
        }
      }

      rc = outer_next(&cx.outer, &gc_key, &gc_data, MDBX_NEXT);
      if (rc != MDBX_SUCCESS) {
        rc = (rc == MDBX_NOTFOUND) ? MDBX_SUCCESS : rc;
        break;
      }
    }
    txn->cursors[FREE_DBI] = cx.outer.next;
  }
  return LOG_IFERR(rc);
}

__cold int mdbx_env_defrag(MDBX_env *env, size_t defrag_atleast, size_t time_atleast_dot16, size_t defrag_enough,
                           size_t time_limit_dot16, intptr_t acceptable_backlash, intptr_t preferred_batch,
                           MDBX_defrag_notify_func progress_callback, void *ctx, MDBX_defrag_result_t *result) {
  if (result)
    memset(result, 0, sizeof(*result));
  if (unlikely(defrag_enough < defrag_atleast) && defrag_enough)
    return LOG_IFERR(MDBX_EINVAL);
  if (unlikely(time_limit_dot16 < time_atleast_dot16) && time_limit_dot16)
    return LOG_IFERR(MDBX_EINVAL);
  if (unlikely(!progress_callback && ctx))
    return LOG_IFERR(MDBX_EINVAL);

  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely(env->flags & MDBX_RDONLY))
    return LOG_IFERR(MDBX_EACCESS);

  dfc_t dfc;
  MDBX_txn *txn = env_owned_wrtxn(env);
  if (txn) {
    rc = check_txn_rw(txn, MDBX_TXN_BLOCKED);
    if (rc == MDBX_SUCCESS && txn->parent)
      rc = MDBX_BAD_TXN;
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
  } else {
    rc = txn_basal_start(txn = env->basal_txn, MDBX_TXN_READWRITE | MDBX_TXN_NOWEAKING);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
    txn->signature = txn_signature;
    txn->userctx = &dfc;
  }

  if (acceptable_backlash < 0)
    acceptable_backlash = CURSOR_STACK_SIZE;
  else
    acceptable_backlash = ((size_t)acceptable_backlash < txn->env->maxgc_large1page) ? (size_t)acceptable_backlash
                                                                                     : txn->env->maxgc_large1page;

  rc = defrag_init(&dfc, txn, defrag_atleast, time_atleast_dot16, defrag_enough, time_limit_dot16, preferred_batch);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  dfc.user_ctx = ctx;
  dfc.user_callback = progress_callback;

  while ((size_t)acceptable_backlash + dfc.payload_pages < txn->geo.first_unallocated && !defrag_discontinued(&dfc)) {
    const pgno_t prev_stumble = dfc.stumble_pgno;
    rc = defrag_cycle(&dfc);

#if MDBX_CHECKING > 1
    pnl_free(dfc.repnl_clone);
    dfc.repnl_clone = nullptr;
#endif /* MDBX_CHECKING > 1 */

    if (MDBX_IS_ERROR(rc)) {
      txn->flags |= MDBX_TXN_ERROR;
      dfc.stopping_reasons |= MDBX_defrag_error;
      break;
    }

    dfc.stumble_retry = (dfc.stumble_pgno && dfc.stumble_pgno == prev_stumble) ? dfc.stumble_retry + 1 : 0;
    if (dfc.stumble_retry > 3) {
      NOTICE("bailout since stucked (unable to move) at %u in a few retries", dfc.stumble_pgno);
      rc = txn->dbs[FREE_DBI].items ? MDBX_LAGGARD_READER : MDBX_RESULT_TRUE;
      break;
    }

    if ((dfc.stopping_reasons & MDBX_defrag_laggard_reader) != 0 && txn_gc_detent(txn))
      dfc.stopping_reasons -= MDBX_defrag_laggard_reader;

    if (rc != MDBX_SUCCESS) {
      if ((dfc.lp_reserve && pnl_size(dfc.lp_reserve)) || dfc.cycle_pages_scheduled != dfc.cycle_pages_moved)
        txn->flags |= MDBX_TXN_ERROR;
      break;
    } else {
      ASSERT(!dfc.lp_reserve || pnl_size(dfc.lp_reserve) == 0);
      ASSERT(dfc.cycle_pages_scheduled == dfc.cycle_pages_moved);
    }

    rc = txn_basal_checkpoint(txn, MDBX_TXN_NOMETASYNC, nullptr);
    if (unlikely(rc != MDBX_SUCCESS)) {
      dfc.txn = txn = nullptr;
      dfc.stopping_reasons |= MDBX_defrag_error;
      break;
    }
  }

  if (txn && txn->userctx == &dfc) {
    if ((txn->flags & MDBX_TXN_ERROR) == 0) {
      if (defrag_score(&dfc, dfc.last_allocated = txn->geo.first_unallocated) > dfc.cycle_initial_score) {
        rc = txn_basal_commit(txn, nullptr);
        dfc.last_allocated = txn->geo.first_unallocated;
      }
    }
    dfc.txn = nullptr;
    int err = txn_basal_end(txn, true);
    rc = (err != MDBX_SUCCESS && !MDBX_IS_ERROR(rc)) ? err : rc;
  }

  defrag_milestone(&dfc);

  if (result) {
    defrag_result(&dfc, result, 0);
    result->pages_moved = dfc.total_pages_moved;
  }

  defrag_destroy(&dfc);

  if (!MDBX_IS_ERROR(rc)) {
    if (dfc.stopping_reasons)
      rc = (dfc.stopping_reasons == MDBX_defrag_laggard_reader) ? MDBX_LAGGARD_READER : MDBX_RESULT_TRUE;
    if (dfc.last_allocated <= dfc.defrag_enough)
      rc = MDBX_SUCCESS;
  }
  return LOG_IFERR(rc);
}
