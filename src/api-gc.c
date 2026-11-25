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
                                                               : mvcc_snapshot_oldest_rw(txn, false).oldest_txnid;
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
        goto next_record;

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
      if (unlikely(rc != MDBX_SUCCESS))
        break;

    next_record:
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
  dfc.user_ctx = ctx;
  dfc.user_callback = progress_callback;
  dfc.acceptable_backlash = (size_t)acceptable_backlash;
  dfc.batch_adaptive = (dfc.wallclock_detent != 0 && preferred_batch == 0);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  /* The cycles go on while the residual exceeds what the caller accepts, and one more time when the last cycle left
   * free pages at the end of the database: they are cut off when the next cycle loads the GC. */
  while (!defrag_discontinued(&dfc) &&
         ((size_t)acceptable_backlash + dfc.payload_pages < txn->geo.first_unallocated || dfc.tail_in_gc)) {
    dfc.cycle_started_timestamp = osal_monotime();
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

    /* The end of the data at the start of the cycle is the effect of the cycles before it, and a cycle that reserved a
     * run of free pages for a large page prepared the move of that page in the next cycle: while any of these is new,
     * the walk makes progress. The effect of a cycle shows at the start of the next one, so the second cycle in a row
     * without progress may still bring the effect of the first, and the third one confirms the stall. */
    if (rc == MDBX_SUCCESS) {
      if (txn->geo.first_unallocated < dfc.least_allocated) {
        dfc.least_allocated = txn->geo.first_unallocated;
        dfc.no_progress = 0;
        dfc.idle_cycles = 0;
        dfc.no_progress_stumbles = 0;
      } else if (dfc.cycle_span_reserved) {
        dfc.no_progress = 0;
        dfc.idle_cycles = 0;
        dfc.no_progress_stumbles = 0;
      } else {
        dfc.no_progress += 1;
        dfc.idle_cycles = (dfc.cycle_pages_scheduled == 0) ? dfc.idle_cycles + 1 : 0;
        dfc.no_progress_stumbles |= dfc.cycle_stumbles;
      }
    }

    /* The residual is within what the caller accepts: the defragmentation ends after the commit of the cycle, as after
     * any cycle, and the walk of this one is not judged. */
    const bool accepted = (size_t)acceptable_backlash + dfc.payload_pages >= txn->geo.first_unallocated;
    if (rc == MDBX_SUCCESS && !accepted && (dfc.no_progress >= 3 || dfc.idle_cycles >= 2)) {
      if (!dfc.in_region && (dfc.no_progress_stumbles & defrag_stumbles_between_edges)) {
        /* Places between the goal edge and the pages can cure what the cycles stumbled on: there is no place or no run
         * below the goal edge, although free pages remain. */
        NOTICE("no place below the goal edge in %u cycles without progress, the pages also take places between the "
               "goal edge and themselves",
               dfc.no_progress);
        dfc.in_region = true;
        dfc.no_progress = 0;
        dfc.idle_cycles = 0;
        dfc.no_progress_stumbles = 0;
      } else {
        /* What the cycles stumbled on tells a limit of the trees from an obstacle that must clear. */
        const bool held_by_limit = (dfc.no_progress_stumbles & defrag_stumbles_limit) != 0;
        NOTICE("bailout after %u cycles without progress, since %s", dfc.no_progress,
               held_by_limit ? "the structure of the trees allows no more pages to be moved"
                             : "the walk does not get through or the pages move and come back");
        /* The cycles have loaded the GC and cleared the records they could reclaim, so the ones left are kept by
         * reading transactions: their pages are free, but cannot be used yet. */
        if (txn->dbs[FREE_DBI].items)
          rc = MDBX_LAGGARD_READER;
        else {
          if (!held_by_limit)
            /* No reader holds the pages, and the trees allow more pages to be moved, but the walk does not get
             * through, or the pages move and come back: the walk can go on so for ever, and no limit of the trees
             * would stop it. A defect of the walk. */
            dfc.stopping_reasons |= MDBX_defrag_error;
          rc = MDBX_RESULT_TRUE;
        }
        break;
      }
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

    if (dfc.completed)
      /* всё reclaimable возвращено в свободные страницы: работа завершена */
      break;

    /* Адаптивный размер батча: при заданном лимите времени подстраиваем
     * порцию так, чтобы цикл занимал около 1/8 оставшегося времени. Это
     * делает остановку по лимиту граничной между циклами и сохраняет
     * промежуточный результат через чекпоинт каждого цикла. */
    if (dfc.batch_adaptive && dfc.cycle != 0) {
      const uint64_t now = osal_monotime();
      const uint64_t cycle_spent = now - dfc.cycle_started_timestamp;
      if (cycle_spent && dfc.wallclock_detent > now) {
        /* aim: a cycle takes about 1/8 of the remaining time budget */
        const uint64_t remaining = dfc.wallclock_detent - now;
        uint64_t target;
        if (cycle_spent > (remaining >> 3))
          target = dfc.move_batch_size >> 1;
        else
          target =
              dfc.move_batch_size + (dfc.move_batch_size * ((remaining >> 3) - cycle_spent)) / ((cycle_spent << 2) + 1);
        if (target < 1024)
          target = 1024;
        if (target > dfc.payload_pages)
          target = dfc.payload_pages;
        dfc.move_batch_size = (pgno_t)target;
      }
    }
  }

  if (txn && txn->userctx == &dfc) {
    if ((txn->flags & MDBX_TXN_ERROR) == 0) {
      if (defrag_score(&dfc, dfc.last_allocated = txn->geo.first_unallocated) > dfc.cycle_initial_score) {
        /* The success of this commit is not the verdict of the defragmentation: the verdict of the last cycle stays. */
        const int err = txn_basal_commit(txn, nullptr);
        dfc.last_allocated = txn->geo.first_unallocated;
        if (unlikely(err != MDBX_SUCCESS))
          rc = err;
      }
    }
    int err = txn_basal_end(txn, true);
    dfc.txn = txn = nullptr;
    rc = (err != MDBX_SUCCESS && !MDBX_IS_ERROR(rc)) ? err : rc;
  }

  defrag_milestone(&dfc);

bailout:
  defrag_result(&dfc, result, 0);
  if (result)
    result->pages_moved = dfc.total_pages_moved;

  defrag_destroy(&dfc);
  if (txn && txn->userctx == &dfc)
    txn_basal_end(txn, true);

  if (!MDBX_IS_ERROR(rc)) {
    if (dfc.completed)
      /* всё reclaimable возвращено: работа выполнена полностью */
      rc = MDBX_SUCCESS;
    else if (dfc.stopping_reasons)
      rc = (dfc.stopping_reasons == MDBX_defrag_laggard_reader) ? MDBX_LAGGARD_READER : MDBX_RESULT_TRUE;
    if (dfc.last_allocated <= dfc.defrag_enough)
      rc = MDBX_SUCCESS;
  }
  return LOG_IFERR(rc);
}
