/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2025-2026

#include "internals.h"

int mdbx_gc_info(MDBX_txn *txn, MDBX_gc_info_t *info, size_t bytes, MDBX_gc_iter_func iter_func, void *iter_ctx) {
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

  info->pages_gc += txn->dbs[FREE_DBI].branch_pages;
  info->pages_gc += txn->dbs[FREE_DBI].leaf_pages;
  info->pages_gc += txn->dbs[FREE_DBI].large_pages;

  if ((txn->flags & txn_ro_flat) == 0) {
    const size_t workset = txn->wr.loose_count + pnl_size(txn->wr.repnl);
    info->gc_reclaimable.pages += workset;
    info->pages_gc += workset + pnl_size(txn->wr.retired_pages) - /* retired_stored */ 0;
  }

  cursor_couple_t cx;
  rc = cursor_init(&cx.outer, txn, FREE_DBI);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  const txnid_t reclaiming_detent = (txn->flags & txn_ro_flat) ? mvcc_shapshot_oldest_ro(txn, false).oldest_txnid
                                                               : mvcc_shapshot_oldest_rw(txn).oldest_txnid;
  MDBX_val gc_key, gc_data;
  rc = outer_first(&cx.outer, &gc_key, &gc_data);
  if (rc == MDBX_SUCCESS) {
    cx.inner.cursor.next = txn->cursors[FREE_DBI];
    txn->cursors[FREE_DBI] = &cx.inner.cursor;
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
      const bool is_reclaimable = id < reclaiming_detent;
      info->pages_gc += len;
      if (is_reclaimable)
        info->gc_reclaimable.pages += len;
      for (size_t span, i = 1; i <= len; i += span) {
        const size_t pgno = glr.pnl[i];
        span = 1;
        while (i + span <= len && MDBX_PNL_CONTIGUOUS(pgno, glr.pnl[i + span], span))
          ++span;
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
