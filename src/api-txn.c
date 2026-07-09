/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

static void txn_latency_init(MDBX_commit_latency *latency, struct commit_timestamp *ts) {
  uint64_t timestamp = 0;
  if (latency) {
    timestamp = osal_monotime();
    memset(latency, 0, sizeof(*latency));
  }
  ts->prep = ts->gc = ts->audit = ts->write = ts->sync = ts->start = timestamp;
  ts->gc_cpu = 0;
}

static void txn_latency_done(MDBX_commit_latency *latency, struct commit_timestamp *ts) {
  if (latency) {
    ASSERT(ts->start);
    latency->preparation = (ts->prep > ts->start) ? osal_monotime_to_16dot16(ts->prep - ts->start) : 0;
    latency->gc_wallclock = (ts->gc > ts->prep) ? osal_monotime_to_16dot16(ts->gc - ts->prep) : 0;
    latency->gc_cputime = ts->gc_cpu ? osal_monotime_to_16dot16(ts->gc_cpu) : 0;
    latency->audit = (ts->audit > ts->gc) ? osal_monotime_to_16dot16(ts->audit - ts->gc) : 0;
    latency->write = (ts->write > ts->audit) ? osal_monotime_to_16dot16(ts->write - ts->audit) : 0;
    latency->sync = (ts->sync > ts->write) ? osal_monotime_to_16dot16(ts->sync - ts->write) : 0;
    const uint64_t ts_end = osal_monotime();
    latency->ending = (ts_end > ts->sync) ? osal_monotime_to_16dot16(ts_end - ts->sync) : 0;
    latency->whole = osal_monotime_to_16dot16_noUnderflow(ts_end - ts->start);
  }
}

#ifdef __SANITIZE_THREAD__
/* LY: avoid tsan-trap by txn, mm_last_pg and geo.first_unallocated */
__attribute__((__no_sanitize_thread__, __noinline__))
#endif
int mdbx_txn_straggler(const MDBX_txn *txn, int *percent) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED - MDBX_TXN_PARKED);
  if (likely(rc == MDBX_SUCCESS))
    rc = check_env(txn->env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR((rc > 0) ? -rc : rc);

  if (unlikely((txn->flags & txn_ro_flat) == 0)) {
    if (percent)
      *percent = (int)((txn->geo.first_unallocated * UINT64_C(100) + txn->geo.end_pgno / 2) / txn->geo.end_pgno);
    return 0;
  }

  txnid_t lag;
  troika_t troika = meta_tap(txn->env);
  do {
    const meta_ptr_t head = meta_recent(txn->env, &troika);
    if (percent) {
      const pgno_t maxpg = head.ptr_v->geometry.now;
      *percent = (int)((head.ptr_v->geometry.first_unallocated * UINT64_C(100) + maxpg / 2) / maxpg);
    }
    lag = (head.txnid - txn->txnid) / xMDBX_TXNID_STEP;
  } while (unlikely(meta_should_retry(txn->env, &troika)));

  return (lag > INT_MAX) ? INT_MAX : (int)lag;
}

MDBX_env *mdbx_txn_env(const MDBX_txn *txn) {
  if (unlikely(!txn || txn->signature != txn_signature || txn->env->signature.weak != env_signature))
    return nullptr;
  return txn->env;
}

uint64_t mdbx_txn_id(const MDBX_txn *txn) {
  if (unlikely(!txn || txn->signature != txn_signature))
    return 0;
  return txn->txnid;
}

MDBX_txn_flags_t mdbx_txn_flags(const MDBX_txn *txn) {
  STATIC_ASSERT(
      (MDBX_TXN_INVALID & (MDBX_TXN_FINISHED | MDBX_TXN_ERROR | MDBX_TXN_DIRTY | MDBX_TXN_SPILLS | MDBX_TXN_HAS_CHILD |
                           txn_gc_drained | txn_shrink_allowed | txn_rw_begin_flags | txn_ro_begin_flags)) == 0);
  if (unlikely(!txn || txn->signature != txn_signature))
    return MDBX_TXN_INVALID;
  ASSERT(0 == (int)(txn->flags & MDBX_TXN_INVALID));

  MDBX_txn_flags_t flags = txn->flags;
  if (F_ISSET(flags, MDBX_TXN_PARKED | txn_ro_flat) && txn->ro.slot &&
      safe64_read(&txn->ro.slot->tid) == MDBX_TID_TXN_OUSTED)
    flags |= MDBX_TXN_OUSTED;
  if (flags & txn_ro_nested)
    flags += MDBX_TXN_RDONLY - txn_ro_nested;
  return flags;
}

int mdbx_txn_reset(MDBX_txn *txn) {
  int rc = check_txn(txn, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  rc = check_env(txn->env, false);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  /* This call is only valid for read-only txns */
  if (unlikely((txn->flags & txn_ro_flat) == 0))
    return LOG_IFERR(MDBX_EINVAL);

  return LOG_IFERR(txn_ro_reset(txn));
}

int mdbx_txn_break(MDBX_txn *txn) {
  do {
    int rc = check_txn(txn, 0);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
    txn->flags |= MDBX_TXN_ERROR;
    txn = txn->nested;
  } while (txn);
  return MDBX_SUCCESS;
}

int mdbx_txn_abort_ex(MDBX_txn *txn, MDBX_commit_latency *latency) {
  struct commit_timestamp ts;
  txn_latency_init(latency, &ts);

  int rc = check_txn(txn, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  rc = check_env(txn->env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

#if MDBX_TXN_CHECKOWNER
  if ((txn->flags & (txn_ro_flat | MDBX_NOSTICKYTHREADS)) == MDBX_NOSTICKYTHREADS &&
      unlikely(txn->owner != osal_thread_self())) {
    mdbx_txn_break(txn);
    return LOG_IFERR(MDBX_THREAD_MISMATCH);
  }
#endif /* MDBX_TXN_CHECKOWNER */

  if (txn->nested) {
    /* more checks for middle-point abortion case */
    mdbx_txn_abort(txn->nested);
    cASSERT0(txn, !txn->nested);
  }

  rc = txn_abort(txn, latency);
  txn_latency_done(latency, &ts);
  return LOG_IFERR(rc);
}

int mdbx_txn_park(MDBX_txn *txn, bool autounpark) {
  STATIC_ASSERT(MDBX_TXN_BLOCKED > MDBX_TXN_ERROR);
  int rc = check_txn(txn, MDBX_TXN_BLOCKED - MDBX_TXN_ERROR);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  rc = check_env(txn->env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely((txn->flags & txn_ro_flat) == 0))
    return LOG_IFERR(MDBX_TXN_INVALID);

  return LOG_IFERR(txn_ro_park(txn, autounpark));
}

int mdbx_txn_unpark(MDBX_txn *txn, bool restart_if_ousted) {
  STATIC_ASSERT(MDBX_TXN_BLOCKED > MDBX_TXN_PARKED + MDBX_TXN_ERROR);
  int rc = check_txn(txn, MDBX_TXN_BLOCKED - MDBX_TXN_PARKED - MDBX_TXN_ERROR);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  rc = check_env(txn->env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely(!F_ISSET(txn->flags, txn_ro_flat | MDBX_TXN_PARKED)))
    return MDBX_SUCCESS;

  rc = txn_ro_unpark(txn);
  if (likely(rc != MDBX_OUSTED) || !restart_if_ousted)
    return LOG_IFERR(rc);

  cASSERT0(txn, txn->flags & MDBX_TXN_FINISHED);
  rc = txn_ro_start(txn, false);
  return (rc == MDBX_SUCCESS) ? MDBX_RESULT_TRUE : LOG_IFERR(rc);
}

int mdbx_txn_refresh(MDBX_txn *txn) {
  int rc = check_txn(txn, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  rc = check_env(txn->env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely((txn->flags & txn_ro_flat) == 0))
    return LOG_IFERR(MDBX_EINVAL);

  if ((txn->flags & MDBX_TXN_FINISHED) == 0) {
    if (recent_committed_txnid(txn->env) == txn->txnid)
      return MDBX_RESULT_TRUE;
    rc = txn_ro_reset(txn);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
  }

  return LOG_IFERR(txn_ro_start(txn, false));
}

int mdbx_txn_renew(MDBX_txn *txn) {
  int rc = check_txn(txn, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  rc = check_env(txn->env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely((txn->flags & txn_ro_flat) == 0))
    return LOG_IFERR(MDBX_EINVAL);

  if (unlikely((txn->flags & MDBX_TXN_FINISHED) == 0)) {
    rc = txn_ro_reset(txn);
    if (unlikely(rc != MDBX_SUCCESS))
      return rc;
  }

  rc = txn_ro_start(txn, false);
  if (rc == MDBX_SUCCESS) {
    tASSERT1(txn, txn->owner == (txn->flags & MDBX_NOSTICKYTHREADS) ? 0 : osal_thread_self());
    DEBUG("renew txn %" PRIaTXN "%c %p on env %p, root page %" PRIaPGNO "/%" PRIaPGNO, txn->txnid,
          (txn->flags & txn_ro_flat) ? 'r' : 'w', __Wpedantic_format_voidptr(txn), __Wpedantic_format_voidptr(txn->env),
          txn->dbs[MAIN_DBI].root, txn->dbs[FREE_DBI].root);
  }
  return LOG_IFERR(rc);
}

int mdbx_txn_set_userctx(MDBX_txn *txn, void *ctx) {
  int rc = check_txn(txn, 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  txn->userctx = ctx;
  return MDBX_SUCCESS;
}

void *mdbx_txn_get_userctx(const MDBX_txn *txn) { return check_txn(txn, MDBX_TXN_FINISHED) ? nullptr : txn->userctx; }

int mdbx_txn_begin_ex(MDBX_env *env, MDBX_txn *parent, MDBX_txn_flags_t flags, MDBX_txn **ret, void *context) {
  if (unlikely(!ret))
    return LOG_IFERR(MDBX_EINVAL);
  *ret = nullptr;

  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely(env->flags & MDBX_RDONLY & ~flags)) /* write txn in RDONLY env */
    return LOG_IFERR(MDBX_EACCESS);

  MDBX_txn *txn = nullptr;
  if (!parent) {
    if (flags & MDBX_TXN_RDONLY) {
      if (unlikely(flags & ~txn_ro_begin_flags))
        return LOG_IFERR(MDBX_EINVAL);
      txn = txn_alloc(txn_ro_flat, env);
      if (unlikely(!txn))
        return LOG_IFERR(MDBX_ENOMEM);
      rc = txn_ro_start(txn, F_ISSET(flags, MDBX_TXN_RDONLY_PREPARE));
      if (unlikely(rc != MDBX_SUCCESS)) {
        txn_ro_free(txn);
        return LOG_IFERR(rc);
      }
    } else {
      if (unlikely(flags & ~txn_rw_begin_flags))
        return LOG_IFERR(MDBX_EINVAL);
      rc = txn_basal_start(txn = env->basal_txn, flags);
      if (unlikely(rc != MDBX_SUCCESS))
        return LOG_IFERR(rc);
    }
  } else {
    if (unlikely(flags & ~(MDBX_TXN_READWRITE | MDBX_TXN_RDONLY)))
      return LOG_IFERR(MDBX_EINVAL);

    rc = check_txn(parent, MDBX_TXN_BLOCKED - MDBX_TXN_PARKED);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);

    if (unlikely(parent->flags & txn_ro_both)) {
      ERROR("%s", "Could not start a nested transaction from the flat read-only parent");
      return LOG_IFERR(MDBX_BAD_TXN);
    }

    if (unlikely(parent->env != env))
      return LOG_IFERR(MDBX_EINVAL);

    if (MDBX_ENABLE_FAKE_NESTED_READONLY_TRANSACTIONS && flags == MDBX_TXN_RDONLY) {
      txn = txn_nested_fakero_begin(parent);
      rc = txn ? MDBX_SUCCESS : MDBX_EINVAL;
    } else {
      rc = txn_nested_create(parent, (flags & MDBX_TXN_RDONLY) != 0);
      txn = parent->nested;
      if (unlikely(rc != MDBX_SUCCESS))
        return LOG_IFERR(rc);
    }
  }

  txn->signature = txn_signature;
  txn->userctx = context;

  if (F_ISSET(flags, MDBX_TXN_RDONLY_PREPARE))
    eASSERT0(env, txn->flags == (txn_ro_flat | MDBX_TXN_FINISHED));
  else if ((flags & txn_ro_flat) != 0 && !parent)
    eASSERT0(env, (txn->flags & ~(MDBX_NOSTICKYTHREADS | txn_ro_flat | MDBX_WRITEMAP |
                                  /* Win32: SRWL flag */ txn_shrink_allowed)) == 0);
  else {
    if (MDBX_ENABLE_FAKE_NESTED_READONLY_TRANSACTIONS)
      eASSERT0(env, (txn->flags & ~(MDBX_NOSTICKYTHREADS | MDBX_WRITEMAP | txn_shrink_allowed | txn_may_have_cursors |
                                    MDBX_NOMETASYNC | MDBX_SAFE_NOSYNC | MDBX_TXN_SPILLS | txn_ro_nested |
                                    MDBX_TXN_DIRTY | txn_gc_drained)) == 0);
    else {
      eASSERT0(env, (txn->flags & ~(MDBX_NOSTICKYTHREADS | MDBX_WRITEMAP | txn_shrink_allowed | txn_may_have_cursors |
                                    MDBX_NOMETASYNC | MDBX_SAFE_NOSYNC | MDBX_TXN_SPILLS | txn_ro_nested)) == 0);
      eASSERT0(env, !txn->wr.spilled.list && !txn->wr.spilled.least_removed);
    }
    if (CHECKS2_ENABLED())
      ENSURE_OBJ(txn, audit_ex(txn, 0, false) == 0);
  }

  *ret = txn;
  DEBUG("begin txn %" PRIaTXN "%c %p on env %p, root page %" PRIaPGNO "/%" PRIaPGNO, txn->txnid,
        (flags & txn_ro_flat) ? 'r' : 'w', __Wpedantic_format_voidptr(txn), __Wpedantic_format_voidptr(env),
        txn->dbs[MAIN_DBI].root, txn->dbs[FREE_DBI].root);
  return MDBX_SUCCESS;
}

int mdbx_txn_commit_embark_read(MDBX_txn **ptxn, MDBX_commit_latency *latency) {
  struct commit_timestamp ts;
  txn_latency_init(latency, &ts);

  if (unlikely(!ptxn))
    return LOG_IFERR(MDBX_EINVAL);

  MDBX_txn *wtxn = *ptxn;
  int rc = check_txn(wtxn, MDBX_TXN_BLOCKED - MDBX_TXN_PARKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  MDBX_env *const env = wtxn->env;
  rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

#if MDBX_TXN_CHECKOWNER
  if ((wtxn->flags & MDBX_NOSTICKYTHREADS) && wtxn == env->basal_txn && unlikely(wtxn->owner != osal_thread_self())) {
    mdbx_txn_break(wtxn);
    return LOG_IFERR(MDBX_THREAD_MISMATCH);
  }
#endif /* MDBX_TXN_CHECKOWNER */

  if (unlikely(wtxn->flags & txn_ro_both))
    goto done;

  if (unlikely(wtxn->nested)) {
    /* more checks for middle-point committing case */
    rc = mdbx_txn_commit_ex(wtxn->nested, nullptr);
    cASSERT0(wtxn, !wtxn->nested);
    if (unlikely(rc != MDBX_SUCCESS)) {
      *ptxn = nullptr;
      mdbx_txn_abort(wtxn);
      goto done;
    }
  }

  MDBX_txn *rtxn = nullptr;
  if (wtxn == env->basal_txn) {
    void *const preserved_context = wtxn->userctx;
    rc = txn_basal_commit(wtxn, latency ? &ts : nullptr);
    if (likely(rc == MDBX_SUCCESS)) {
      rc = txn_basal_end(wtxn, /* завершение транзакции без освобождения блокировки */ false);
      if (likely(rc == MDBX_SUCCESS)) {
        rtxn = txn_alloc(txn_ro_flat, env);
        rc = likely(rtxn) ? txn_ro_start(rtxn, false) : MDBX_ENOMEM;
      }
    }

    const int err = txn_basal_end(wtxn, /* освобождение блокировки */ true);
    if (unlikely(err != MDBX_SUCCESS) && rc == MDBX_SUCCESS)
      rc = err;
    if (likely(rc == MDBX_SUCCESS)) {
      rtxn->userctx = preserved_context;
      rtxn->signature = txn_signature;
    } else if (rtxn) {
      txn_ro_free(rtxn);
      rtxn = nullptr;
    }
  } else {
    rc = txn_nested_checkpoint(wtxn, latency ? &ts : nullptr);
    if (likely(rc == MDBX_SUCCESS)) {
      rtxn = wtxn;
      rtxn->flags |= txn_ro_nested;
      cASSERT0(rtxn, rtxn->env->txn == rtxn);
    }
  }
  *ptxn = rtxn;

done:
  txn_latency_done(latency, &ts);
  return LOG_IFERR(rc);
}

int mdbx_txn_checkpoint(MDBX_txn *txn, MDBX_txn_flags_t weakening_durability, MDBX_commit_latency *latency) {
  struct commit_timestamp ts;
  txn_latency_init(latency, &ts);

  int rc = check_txn(txn, MDBX_TXN_BLOCKED - MDBX_TXN_PARKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely(weakening_durability & ~(MDBX_TXN_NOMETASYNC | MDBX_TXN_NOSYNC | MDBX_TXN_NOWEAKING)))
    return LOG_IFERR(MDBX_EINVAL);

  MDBX_env *const env = txn->env;
  rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

#if MDBX_TXN_CHECKOWNER
  if ((txn->flags & MDBX_NOSTICKYTHREADS) && txn == env->basal_txn && unlikely(txn->owner != osal_thread_self())) {
    mdbx_txn_break(txn);
    return LOG_IFERR(MDBX_THREAD_MISMATCH);
  }
#endif /* MDBX_TXN_CHECKOWNER */

  if (unlikely(txn->nested)) {
    /* more checks for middle-point committing case */
    rc = mdbx_txn_commit_ex(txn->nested, nullptr);
    cASSERT0(txn, !txn->nested);
    if (unlikely(rc != MDBX_SUCCESS)) {
      mdbx_txn_abort(txn);
      goto done;
    }
  }

  if ((txn->flags & MDBX_TXN_DIRTY) == 0)
    return MDBX_RESULT_TRUE;

  if (txn == env->basal_txn) {
    rc = txn_basal_checkpoint(txn, weakening_durability, latency ? &ts : nullptr);
  } else {
    if (unlikely(weakening_durability != MDBX_TXN_NOWEAKING))
      return LOG_IFERR(MDBX_EINVAL);
    rc = txn_nested_checkpoint(txn, latency ? &ts : nullptr);
  }

done:
  txn_latency_done(latency, &ts);
  return LOG_IFERR(rc);
}

int mdbx_txn_commit_ex(MDBX_txn *txn, MDBX_commit_latency *latency) {
  STATIC_ASSERT(MDBX_TXN_FINISHED == MDBX_TXN_BLOCKED - MDBX_TXN_HAS_CHILD - MDBX_TXN_ERROR - MDBX_TXN_PARKED);

  struct commit_timestamp ts;
  txn_latency_init(latency, &ts);

  int rc = check_txn(txn, MDBX_TXN_FINISHED);
  if (unlikely(rc != MDBX_SUCCESS)) {
    if (rc == MDBX_BAD_TXN && F_ISSET(txn->flags, MDBX_TXN_FINISHED | txn_ro_flat)) {
      rc = MDBX_RESULT_TRUE;
      mdbx_txn_abort(txn);
      goto done;
    }
    return LOG_IFERR(rc);
  }

  MDBX_env *const env = txn->env;
  rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

#if MDBX_TXN_CHECKOWNER
  if ((txn->flags & MDBX_NOSTICKYTHREADS) && txn == env->basal_txn && unlikely(txn->owner != osal_thread_self())) {
    mdbx_txn_break(txn);
    return LOG_IFERR(MDBX_THREAD_MISMATCH);
  }
#endif /* MDBX_TXN_CHECKOWNER */

  if (unlikely(txn->nested)) {
    /* more checks for middle-point committing case */
    rc = mdbx_txn_commit_ex(txn->nested, nullptr);
    cASSERT0(txn, !txn->nested);
    if (unlikely(rc != MDBX_SUCCESS)) {
      mdbx_txn_abort(txn);
      goto done;
    }
  }

  rc = txn_commit(txn, latency, latency ? &ts : nullptr);
done:
  txn_latency_done(latency, &ts);
  return LOG_IFERR(rc);
}

int mdbx_txn_info(const MDBX_txn *txn, MDBX_txn_info *info, bool scan_rlt) {
  int rc = check_txn(txn, MDBX_TXN_FINISHED);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely(!info))
    return LOG_IFERR(MDBX_EINVAL);

  MDBX_env *const env = txn->env;
#if MDBX_ENV_CHECKPID
  if (unlikely(env->pid != osal_getpid())) {
    env->flags |= ENV_FATAL_ERROR;
    return LOG_IFERR(MDBX_PANIC);
  }
#endif /* MDBX_ENV_CHECKPID */

  info->txn_id = txn->txnid;
  info->txn_space_used = pgno2bytes(env, txn->geo.first_unallocated);
  info->txn_pget =
#if MDBX_ENABLE_PGET_STAT
      txn->ops_pget
#else
      0
#endif /* MDBX_ENABLE_PGET_STAT */
      ;

  if (txn->flags & txn_ro_flat) {
    meta_ptr_t head;
    uint64_t head_retired;
    troika_t troika = meta_tap(env);
    do {
      /* fetch info from volatile head */
      head = meta_recent(env, &troika);
      head_retired = unaligned_peek_u64_volatile(4, head.ptr_v->pages_retired);
      info->txn_space_limit_soft = pgno2bytes(env, head.ptr_v->geometry.now);
      info->txn_space_limit_hard = pgno2bytes(env, head.ptr_v->geometry.upper);
      info->txn_space_leftover = pgno2bytes(env, head.ptr_v->geometry.now - head.ptr_v->geometry.first_unallocated);
    } while (unlikely(meta_should_retry(env, &troika)));

    info->txn_reader_lag = head.txnid - info->txn_id;
    info->txn_space_dirty = info->txn_space_retired = 0;
    uint64_t reader_snapshot_pages_retired = 0;
    if (txn->ro.slot &&
        ((txn->flags & MDBX_TXN_PARKED) == 0 || safe64_read(&txn->ro.slot->tid) != MDBX_TID_TXN_OUSTED) &&
        head_retired >
            (reader_snapshot_pages_retired = atomic_load64(&txn->ro.slot->snapshot_pages_retired, mo_Relaxed))) {
      info->txn_space_dirty = info->txn_space_retired =
          pgno2bytes(env, (pgno_t)(head_retired - reader_snapshot_pages_retired));

      size_t retired_next_reader = 0;
      lck_t *const lck = env->lck_mmap.lck;
      if (scan_rlt && info->txn_reader_lag > 1 && lck) {
        /* find next more recent reader */
        txnid_t next_reader = head.txnid;
        const size_t snap_nreaders = atomic_load32(&lck->rdt_length, mo_AcquireRelease);
        for (size_t i = 0; i < snap_nreaders; ++i) {
        retry:
          if (atomic_load_pid(&lck->rdt[i].pid, mo_AcquireRelease)) {
            jitter4testing(true);
            const uint64_t snap_tid = safe64_read(&lck->rdt[i].tid);
            const txnid_t snap_txnid = safe64_read(&lck->rdt[i].txnid);
            const uint64_t snap_retired = atomic_load64(&lck->rdt[i].snapshot_pages_retired, mo_AcquireRelease);
            if (unlikely(snap_retired != atomic_load64(&lck->rdt[i].snapshot_pages_retired, mo_Relaxed)) ||
                snap_txnid != safe64_read(&lck->rdt[i].txnid) || snap_tid != safe64_read(&lck->rdt[i].tid))
              goto retry;
            if (snap_txnid <= txn->txnid) {
              retired_next_reader = 0;
              break;
            }
            if (snap_txnid < next_reader && snap_tid >= MDBX_TID_TXN_OUSTED) {
              next_reader = snap_txnid;
              retired_next_reader = pgno2bytes(
                  env, (pgno_t)(snap_retired - atomic_load64(&txn->ro.slot->snapshot_pages_retired, mo_Relaxed)));
            }
          }
        }
      }
      info->txn_space_dirty = retired_next_reader;
    }
  } else {
    info->txn_space_limit_soft = pgno2bytes(env, txn->geo.now);
    info->txn_space_limit_hard = pgno2bytes(env, txn->geo.upper);
    info->txn_space_retired =
        pgno2bytes(env, txn->nested ? (size_t)txn->wr.retired_pages : pnl_size(txn->wr.retired_pages));
    info->txn_space_leftover = pgno2bytes(env, txn->wr.dirtyroom);
    info->txn_space_dirty =
        pgno2bytes(env, txn->wr.dirtylist ? txn->wr.dirtylist->pages_including_loose
                                          : (txn->wr.writemap_dirty_npages + txn->wr.writemap_spilled_npages));
    info->txn_reader_lag = INT64_MAX;
    lck_t *const lck = env->lck_mmap.lck;
    if (scan_rlt && lck) {
      txnid_t oldest_reading = txn->txnid;
      const size_t snap_nreaders = atomic_load32(&lck->rdt_length, mo_AcquireRelease);
      if (snap_nreaders) {
        txn_gc_detent(txn);
        oldest_reading = txn->env->gc.detent;
        if (oldest_reading == txn->wr.troika.txnid[txn->wr.troika.recent]) {
          /* Если самый старый используемый снимок является предыдущим, т.е. непосредственно предшествующим текущей
           * транзакции, то просматриваем таблицу читателей чтобы выяснить действительно ли снимок используется
           * читателями. */
          oldest_reading = txn->txnid;
          for (size_t i = 0; i < snap_nreaders; ++i) {
            if (atomic_load_pid(&lck->rdt[i].pid, mo_Relaxed) &&
                txn->env->gc.detent == safe64_read(&lck->rdt[i].txnid)) {
              oldest_reading = txn->env->gc.detent;
              break;
            }
          }
        }
      }
      info->txn_reader_lag = txn->txnid - oldest_reading;
    }
  }

  return MDBX_SUCCESS;
}

int mdbx_txn_clone(const MDBX_txn *source, MDBX_txn **in_out_clone, void *clone_context) {
  if (unlikely(!in_out_clone))
    return LOG_IFERR(MDBX_EINVAL);

  MDBX_txn *clone = *in_out_clone;
retry:
  osal_memory_fence(mo_AcquireRelease, true);
  int rc = check_txn_anythread(
      source,
      MDBX_TXN_FINISHED |
          /* there is no difficulty, but we do not allow dirty transactions to avoid confusion */ MDBX_TXN_DIRTY);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  if (unlikely(source->parent)) {
    /* do not immediately jump to env->basal_txn,
     * but check for the absence of MDBX_TXN_DIRTY in all parent transactions. */
    source = source->parent;
    goto retry;
  }

  MDBX_env *const env = source->env;
  rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  const txnid_t txnid = source->txnid;
  if (clone) {
    rc = check_txn(clone, 0);
    if (unlikely(rc != MDBX_SUCCESS))
      goto bailout;
    if (unlikely((clone->flags & txn_ro_flat) == 0)) {
      rc = MDBX_EINVAL;
      goto bailout;
    }

    if (clone_context == clone)
      clone_context = clone->userctx;

    if (unlikely((clone->flags & MDBX_TXN_FINISHED) == 0)) {
      rc = txn_ro_reset(clone);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
      goto retry;
    }
  } else {
    clone = txn_alloc(txn_ro_flat | MDBX_TXN_FINISHED, env);
    if (unlikely(!clone))
      return LOG_IFERR(MDBX_ENOMEM);
    clone->signature = txn_signature;
    goto retry;
  }

  rc = txn_ro_clone(source, clone);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  osal_memory_fence(mo_AcquireRelease, true);
  rc = check_txn_anythread(source, MDBX_TXN_FINISHED | MDBX_TXN_DIRTY);
  if (unlikely(rc != MDBX_SUCCESS))
    goto bailout;

  clone->flags &= ~MDBX_TXN_FINISHED;
  clone->userctx = clone_context;
  if (unlikely(txn_basis_snapshot(source) != clone->txnid || txnid != source->txnid)) {
    rc = MDBX_PROBLEM;
    ERROR("unexpected mvcc-tnxid (%" PRIu64 " != %" PRIu64 ") mismatch during cloning of a transaction,"
          " seems the original transaction competitively restarted in another thread",
          txnid, (txnid != source->txnid) ? source->txnid : clone->txnid);
    goto bailout;
  }

  *in_out_clone = clone;
  return MDBX_SUCCESS;

bailout:
  if (clone) {
    if (clone != *in_out_clone)
      txn_ro_free(clone);
    else
      txn_ro_reset(clone);
  }
  return LOG_IFERR(rc);
}

int mdbx_txn_rollback(MDBX_txn *txn) {
  int rc = check_txn(txn, MDBX_TXN_BLOCKED - MDBX_TXN_ERROR - MDBX_TXN_PARKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  txn->flags |= /* avoid merge cursors' state */ MDBX_TXN_ERROR;
  if (txn->flags & txn_may_have_cursors)
    txn_done_cursors(txn);

  if (likely(txn->flags & txn_ro_flat) == 0) {
    if (!txn->parent)
      return LOG_IFERR(txn_basal_rollback(txn));
    else
      return LOG_IFERR(txn_nested_rollback(txn));
  }

  rc = txn_ro_reset(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);
  return LOG_IFERR(txn_ro_start(txn, false));
}

int mdbx_txn_amend(MDBX_txn *rtxn, MDBX_txn **ptxn, MDBX_txn_flags_t flags, void *context) {
  if (unlikely(!ptxn))
    return LOG_IFERR(MDBX_EINVAL);

  if (*ptxn != rtxn)
    *ptxn = nullptr;

  if (unlikely(flags & ~(txn_rw_begin_flags | MDBX_TXN_RDONLY_PREPARE)))
    return LOG_IFERR(MDBX_EINVAL);

  int rc = check_txn(rtxn, MDBX_TXN_BLOCKED - MDBX_TXN_PARKED);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  MDBX_env *const env = rtxn->env;
  rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  if (unlikely((rtxn->flags & txn_ro_flat) == 0))
    return LOG_IFERR(MDBX_BAD_TXN);

  /* avoid recursive deadlock */
  if (env->txn && env->basal_txn->owner == osal_thread_self())
    return LOG_IFERR(MDBX_TXN_OVERLAPPING);

  /* first check that there were no new commits */
  if (recent_committed_txnid(env) != rtxn->txnid)
    return LOG_IFERR(MDBX_RESULT_TRUE);

  STATIC_ASSERT(MDBX_TXN_TRY != MDBX_TXN_PARKED);
  if ((uint32_t)(flags & MDBX_TXN_TRY) == (uint32_t)(rtxn->flags & MDBX_TXN_PARKED)) {
    rc = txn_ro_park(rtxn, true);
    if (unlikely(rc != MDBX_SUCCESS))
      return LOG_IFERR(rc);
  }
  rc = lck_txn_lock(env, (flags & MDBX_TXN_TRY) != 0);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  /* after the lock was acquired we re-check that there were no new commits */
  if (recent_committed_txnid(env) != rtxn->txnid) {
    lck_txn_unlock(env);
    return LOG_IFERR(MDBX_RESULT_TRUE);
  }

  rc = txn_basal_start(env->basal_txn, (flags & ~MDBX_TXN_RDONLY_PREPARE) | txn_rw_already_locked);
  if (unlikely(rc != MDBX_SUCCESS))
    return LOG_IFERR(rc);

  /* A paranoid check that changes will be made on top of the desired data snapshot */
  rc = MDBX_RESULT_TRUE;
  if (txn_basis_snapshot(env->basal_txn) == rtxn->txnid)
    rc = txn_ro_reset(rtxn);
  if (unlikely(rc != MDBX_SUCCESS)) {
    env->basal_txn->flags |= MDBX_TXN_ERROR;
    txn_basal_end(env->basal_txn, true);
    return LOG_IFERR(rc);
  }

  if (!F_ISSET(flags, MDBX_TXN_RDONLY_PREPARE))
    txn_ro_free(rtxn);
  env->basal_txn->userctx = context;
  *ptxn = env->basal_txn;
  return MDBX_SUCCESS;
}
