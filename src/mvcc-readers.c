/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

bsr_t mvcc_bind_slot(MDBX_env *env) {
  eASSERT0(env, env->lck_mmap.lck);
  eASSERT0(env, env->lck->magic_and_version == MDBX_LOCK_MAGIC);
  eASSERT0(env, env->lck->os_and_format == MDBX_LOCK_FORMAT);

  bsr_t result = {lck_rdt_lock(env), nullptr};
  if (unlikely(MDBX_IS_ERROR(result.err)))
    return result;
  if (unlikely(env->flags & ENV_FATAL_ERROR)) {
    lck_rdt_unlock(env);
    result.err = MDBX_PANIC;
    return result;
  }
  if (unlikely(!env->dxb_mmap.base)) {
    lck_rdt_unlock(env);
    result.err = MDBX_EPERM;
    return result;
  }

  if (unlikely(env->registered_reader_pid != env->pid)) {
    result.err = lck_rpid_set(env);
    if (unlikely(result.err != MDBX_SUCCESS)) {
      lck_rdt_unlock(env);
      return result;
    }
    env->registered_reader_pid = env->pid;
  }

  result.err = MDBX_SUCCESS;
  size_t slot, nreaders;
  while (1) {
    nreaders = env->lck->rdt_length.weak;
    for (slot = 0; slot < nreaders; slot++)
      if (!atomic_load_pid(&env->lck->rdt[slot].pid, mo_AcquireRelease))
        break;

    if (likely(slot < env->max_readers))
      break;

    result.err = mvcc_cleanup_dead(env, true, nullptr);
    if (result.err != MDBX_RESULT_TRUE) {
      lck_rdt_unlock(env);
      result.err = (result.err == MDBX_SUCCESS) ? MDBX_READERS_FULL : result.err;
      return result;
    }
  }

  result.slot = &env->lck->rdt[slot];
  /* Claim the reader slot, carefully since other code
   * uses the reader table un-mutexed: First reset the
   * slot, next publish it in lck->rdt_length.  After
   * that, it is safe for mdbx_env_close() to touch it.
   * When it will be closed, we can finally claim it. */
  atomic_store32(&result.slot->pid, 0, mo_AcquireRelease);
  safe64_reset(&result.slot->txnid, true);
  if (slot == nreaders)
    env->lck->rdt_length.weak = (uint32_t)++nreaders;
  result.slot->tid.weak = (env->flags & MDBX_NOSTICKYTHREADS) ? 0 : osal_thread_self();
  atomic_store32(&result.slot->pid, env->pid, mo_AcquireRelease);
  lck_rdt_unlock(env);

  if (likely(env->flags & ENV_TXKEY)) {
    eASSERT0(env, env->registered_reader_pid == env->pid);
    thread_rthc_set(env->me_txkey, result.slot);
  }
  return result;
}

#define NOTHING_CHANGED_SIGNATURE MDBX_TETRAD('N', 'o', 'n', 'e')

MDBX_MAYBE_UNUSED __hot orsi_ro_t mvcc_snapshot_oldest_ro(const MDBX_txn *const txn,
                                                          const bool need_thisprocess_oldest) {
  const uint32_t nothing_changed_signature = NOTHING_CHANGED_SIGNATURE;
  cASSERT0(txn, (txn->flags & txn_ro_flat) != 0);
  orsi_ro_t result;
  lck_t *const lck = txn->env->lck;
  result.nreaders = atomic_load32(&lck->rdt_length, mo_AcquireRelease);
  result.thisprocess_oldest_txnid = result.oldest_txnid = result.recent_txnid = recent_committed_txnid(txn->env);

  if (!need_thisprocess_oldest &&
      nothing_changed_signature == atomic_load32(&lck->rdt_refresh_flag, mo_AcquireRelease)) {
    result.oldest_txnid = atomic_load64(&lck->cached_oldest_txnid, mo_AcquireRelease);
    if (nothing_changed_signature == atomic_load32(&lck->rdt_refresh_flag, mo_AcquireRelease))
      return result;
  }

  for (size_t i = 0; i < result.nreaders; ++i) {
    const mdbx_pid_t pid = atomic_load_pid(&lck->rdt[i].pid, mo_AcquireRelease);
    if (!pid)
      continue;

    jitter4testing(true);
    const txnid_t reader_txnid = safe64_read(&lck->rdt[i].txnid);
    result.oldest_txnid = (result.oldest_txnid > reader_txnid) ? reader_txnid : result.oldest_txnid;
    if (pid == txn->env->registered_reader_pid && result.thisprocess_oldest_txnid > reader_txnid)
      result.thisprocess_oldest_txnid = reader_txnid;
  }

  return result;
}

__hot orsi_rw_t mvcc_snapshot_oldest_rw(const MDBX_txn *const txn) {
  const uint32_t nothing_changed_signature = NOTHING_CHANGED_SIGNATURE;
  cASSERT0(txn, (txn->flags & txn_ro_flat) == 0);
  lck_t *const lck = txn->env->lck;
  uint64_t oldest_retired_pages = lck->cached_oldest_retired.weak;
  const txnid_t prev_oldest = atomic_load64(&lck->cached_oldest_txnid, mo_AcquireRelease);
  const meta_ptr_t steady = meta_prefer_steady(txn->env, &txn->wr.troika);
  orsi_rw_t result = {.steady_txnid = steady.txnid, .oldest_txnid = prev_oldest};
  cASSERT0(txn, steady.txnid <= txn->env->basal_txn->txnid);
  cASSERT0(txn, steady.txnid >= prev_oldest);

  while (nothing_changed_signature != atomic_load32(&lck->rdt_refresh_flag, mo_AcquireRelease)) {
    lck->rdt_refresh_flag.weak = nothing_changed_signature;
    jitter4testing(false);
    const size_t snap_nreaders = atomic_load32(&lck->rdt_length, mo_AcquireRelease);
    result.oldest_txnid = result.steady_txnid;
    oldest_retired_pages = unaligned_peek_u64(4, steady.ptr_c->pages_retired);

    for (size_t i = 0; i < snap_nreaders; ++i) {
    retry:;
      const mdbx_pid_t pid = atomic_load_pid(&lck->rdt[i].pid, mo_AcquireRelease);
      if (!pid)
        continue;
      jitter4testing(true);

      const uint64_t reader_retired = atomic_load64(&lck->rdt[i].snapshot_pages_retired, mo_Relaxed);
      const txnid_t reader_txnid = atomic_load64(&lck->rdt[i].txnid, mo_Relaxed);
      if (unlikely(reader_retired != atomic_load64(&lck->rdt[i].snapshot_pages_retired, mo_AcquireRelease) ||
                   reader_txnid != atomic_load64(&lck->rdt[i].txnid, mo_AcquireRelease))) {
        atomic_yield();
        goto retry;
      }

      if (unlikely(reader_txnid < prev_oldest)) {
        if (unlikely(nothing_changed_signature == atomic_load32(&lck->rdt_refresh_flag, mo_AcquireRelease)) &&
            safe64_reset_compare(&lck->rdt[i].txnid, reader_txnid)) {
          NOTICE("kick stuck reader[%zu of %zu].pid_%zu %" PRIaTXN " < prev-oldest %" PRIaTXN ", steady-txn %" PRIaTXN,
                 i, snap_nreaders, (size_t)pid, reader_txnid, prev_oldest, steady.txnid);
        }
        continue;
      }

      if (reader_txnid < result.oldest_txnid) {
        result.oldest_txnid = reader_txnid;
        oldest_retired_pages = reader_retired;
        if (MDBX_CHECKING < 1 && result.oldest_txnid == prev_oldest)
          break;
      }
    }
  }

  if (result.oldest_txnid != prev_oldest) {
    VERBOSE("update oldest %" PRIaTXN " -> %" PRIaTXN, prev_oldest, result.oldest_txnid);
    cASSERT0(txn, result.oldest_txnid >= lck->cached_oldest_txnid.weak);
    cASSERT0(txn, oldest_retired_pages >= lck->cached_oldest_retired.weak);
    atomic_store64(&lck->cached_oldest_txnid, result.oldest_txnid, mo_Relaxed);
    atomic_store64(&lck->cached_oldest_retired, oldest_retired_pages, mo_Relaxed);
    const meta_ptr_t recent = meta_recent(txn->env, &txn->wr.troika);
    const uint64_t reader_lag = recent.txnid - result.oldest_txnid;
    if (reader_lag > lck->pgops.gc_prof.max_reader_lag)
      lck->pgops.gc_prof.max_reader_lag = (reader_lag < UINT32_MAX) ? (uint32_t)reader_lag : UINT32_MAX;
    const uint64_t recent_retired = unaligned_peek_u64(4, recent.ptr_c->pages_retired);
    if (recent_retired > oldest_retired_pages) {
      const uint64_t retained_pages = recent_retired - oldest_retired_pages;
      if (retained_pages > lck->pgops.gc_prof.max_retained_pages)
        lck->pgops.gc_prof.max_retained_pages = (retained_pages < UINT32_MAX) ? (uint32_t)retained_pages : UINT32_MAX;
    }
  }
  return result;
}

pgno_t mvcc_snapshot_largest(const MDBX_env *env, pgno_t last_used_page) {
  lck_t *const lck = env->lck;
retry:;
  const size_t snap_nreaders = atomic_load32(&lck->rdt_length, mo_AcquireRelease);
  for (size_t i = 0; i < snap_nreaders; ++i) {
    if (atomic_load_pid(&lck->rdt[i].pid, mo_AcquireRelease)) {
      /* jitter4testing(true); */
      const pgno_t snap_pages = atomic_load32(&lck->rdt[i].snapshot_pages_used, mo_Relaxed);
      const txnid_t snap_txnid = safe64_read(&lck->rdt[i].txnid);
      if (unlikely(snap_pages != atomic_load32(&lck->rdt[i].snapshot_pages_used, mo_AcquireRelease) ||
                   snap_txnid != safe64_read(&lck->rdt[i].txnid)))
        goto retry;
      if (last_used_page < snap_pages && snap_txnid <= env->basal_txn->txnid)
        last_used_page = snap_pages;
    }
  }

  return last_used_page;
}

/* Find largest mvcc-snapshot still referenced by this process. */
pgno_t mvcc_largest_this(MDBX_env *env, pgno_t largest) {
  lck_t *const lck = env->lck;
  const size_t snap_nreaders = atomic_load32(&lck->rdt_length, mo_AcquireRelease);
  for (size_t i = 0; i < snap_nreaders; ++i) {
  retry:
    if (atomic_load_pid(&lck->rdt[i].pid, mo_AcquireRelease) == env->registered_reader_pid) {
      /* jitter4testing(true); */
      const pgno_t snap_pages = atomic_load32(&lck->rdt[i].snapshot_pages_used, mo_Relaxed);
      const txnid_t snap_txnid = safe64_read(&lck->rdt[i].txnid);
      if (unlikely(snap_pages != atomic_load32(&lck->rdt[i].snapshot_pages_used, mo_AcquireRelease) ||
                   snap_txnid != safe64_read(&lck->rdt[i].txnid)))
        goto retry;
      if (largest < snap_pages &&
          atomic_load64(&lck->cached_oldest_txnid, mo_AcquireRelease) <=
              /* ignore pending updates */ snap_txnid &&
          snap_txnid <= MAX_TXNID)
        largest = snap_pages;
    }
  }
  return largest;
}

static bool pid_insert(mdbx_pid_t *list, mdbx_pid_t pid) {
  /* binary search of pid in list */
  size_t base = 0;
  size_t cursor = 1;
  int32_t val = 0;
  size_t n = /* length */ list[0];

  while (n > 0) {
    size_t pivot = n >> 1;
    cursor = base + pivot + 1;
    val = pid - list[cursor];

    if (val < 0) {
      n = pivot;
    } else if (val > 0) {
      base = cursor;
      n -= pivot + 1;
    } else {
      /* found, so it's a duplicate */
      return false;
    }
  }

  if (val > 0)
    ++cursor;

  list[0]++;
  for (n = list[0]; n > cursor; n--)
    list[n] = list[n - 1];
  list[n] = pid;
  return true;
}

__cold int mvcc_cleanup_dead(MDBX_env *env, int rdt_locked, int *dead) {
  int rc = check_env(env, true);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  eASSERT0(env, rdt_locked >= 0);
  lck_t *const lck = env->lck_mmap.lck;
  if (unlikely(lck == nullptr)) {
    /* exclusive mode */
    if (dead)
      *dead = 0;
    return MDBX_SUCCESS;
  }

  const size_t snap_nreaders = atomic_load32(&lck->rdt_length, mo_AcquireRelease);
  mdbx_pid_t pidsbuf_onstask[142];
  mdbx_pid_t *const pids = (snap_nreaders < ARRAY_LENGTH(pidsbuf_onstask))
                               ? pidsbuf_onstask
                               : osal_malloc((snap_nreaders + 1) * sizeof(mdbx_pid_t));
  if (unlikely(!pids))
    return MDBX_ENOMEM;

  pids[0] = 0;
  int count = 0;
  for (size_t i = 0; i < snap_nreaders; i++) {
    const mdbx_pid_t pid = atomic_load_pid(&lck->rdt[i].pid, mo_AcquireRelease);
    if (pid == 0)
      continue /* skip empty */;
    if (pid == env->pid)
      continue /* skip self */;
    if (!pid_insert(pids, pid))
      continue /* such pid already processed */;

    int err = lck_rpid_check(env, pid);
    if (err == MDBX_RESULT_TRUE)
      continue /* reader is live */;

    if (err != MDBX_SUCCESS) {
      rc = err;
      break /* lck_rpid_check() failed */;
    }

    /* stale reader found */
    if (!rdt_locked) {
      err = lck_rdt_lock(env);
      if (MDBX_IS_ERROR(err)) {
        rc = err;
        break;
      }

      rdt_locked = -1;
      if (err == MDBX_RESULT_TRUE) {
        /* mutex recovered, the mdbx_ipclock_failed() checked all readers */
        rc = MDBX_RESULT_TRUE;
        break;
      }

      /* a other process may have clean and reused slot, recheck */
      if (lck->rdt[i].pid.weak != (size_t)pid)
        continue;

      err = lck_rpid_check(env, pid);
      if (MDBX_IS_ERROR(err)) {
        rc = err;
        break;
      }

      if (err != MDBX_SUCCESS)
        continue /* the race with other process, slot reused */;
    }

    /* clean it */
    for (size_t ii = i; ii < snap_nreaders; ii++) {
      if (lck->rdt[ii].pid.weak == (size_t)pid) {
        DEBUG("clear stale reader pid %" PRIuPTR " txn %" PRIaTXN, (size_t)pid, lck->rdt[ii].txnid.weak);
        atomic_store32(&lck->rdt[ii].pid, 0, mo_Relaxed);
        atomic_store32(&lck->rdt_refresh_flag, true, mo_AcquireRelease);
        count++;
      }
    }
  }

  if (likely(!MDBX_IS_ERROR(rc)))
    atomic_store64(&lck->readers_check_timestamp, osal_monotime(), mo_Relaxed);

  if (rdt_locked < 0)
    lck_rdt_unlock(env);

  if (pids != pidsbuf_onstask)
    osal_free(pids);

  if (dead)
    *dead = count;
  return rc;
}

__cold bool mvcc_kick_laggards(MDBX_txn *txn, const txnid_t straggler,
                               struct gc_reclaiming_obstacle *optional_obstacle) {
  DEBUG("DB size maxed out by reading #%" PRIaTXN, straggler);
  osal_memory_fence(mo_AcquireRelease, false);
  if (optional_obstacle) {
    optional_obstacle->pid = 0;
    optional_obstacle->tid = 0;
    optional_obstacle->txnid = 0;
  }

  MDBX_env *const env = txn->env;
  MDBX_hsr_func const callback = env->hsr_callback;
  orsi_rw_t orsi;
  bool notify_eof_of_loop = false;
  int retry = 0;

  do {
    env->lck->rdt_refresh_flag.weak = /* force refresh */ true;
    orsi = mvcc_snapshot_oldest_rw(txn);
    eASSERT0(env, orsi.oldest_txnid < env->basal_txn->txnid);
    eASSERT0(env, orsi.oldest_txnid >= straggler);
    eASSERT0(env, orsi.oldest_txnid >= env->lck->cached_oldest_txnid.weak);

    lck_t *const lck = env->lck_mmap.lck;
    if (orsi.oldest_txnid == orsi.steady_txnid || orsi.oldest_txnid > straggler || /* without-LCK mode */ !lck)
      break;

    if (MDBX_IS_ERROR(mvcc_cleanup_dead(env, false, nullptr)))
      break;

    reader_slot_t *stucked = nullptr;
    uint64_t stucked_retired = 0;
    uint64_t stucked_tid = 0;
    for (size_t i = 0; i < lck->rdt_length.weak; ++i) {
      mdbx_pid_t pid;
      reader_slot_t *const slot = &lck->rdt[i];
    retry:;
      const txnid_t slot_snap_mvcc = safe64_read(&slot->txnid);
      if (slot_snap_mvcc == orsi.oldest_txnid && (pid = atomic_load_pid(&slot->pid, mo_AcquireRelease)) != 0) {
        const uint64_t tid = safe64_read(&slot->tid);
        if (tid == MDBX_TID_TXN_PARKED) {
          /* Читающая транзакция была помечена владельцем как "припаркованная",
           * т.е. подлежащая асинхронному прерыванию, либо восстановлению
           * по активности читателя.
           *
           * Если первый CAS(slot->tid) будет успешным, то
           * safe64_reset_compare() безопасно очистит txnid, либо откажется
           * из-за того что читатель сбросил и/или перезапустил транзакцию.
           * При этом читатеть может не заметить вытестения, если приступит
           * к завершению транзакции. Все эти исходы нас устраивют.
           *
           * Если первый CAS(slot->tid) будет НЕ успешным, то значит читатеть
           * восстановил транзакцию, либо завершил её, либо даже освободил слот.
           */
          bool ousted =
#if MDBX_64BIT_CAS
              atomic_cas64(&slot->tid, MDBX_TID_TXN_PARKED, MDBX_TID_TXN_OUSTED);
#else
              atomic_cas32(&slot->tid.low, (uint32_t)MDBX_TID_TXN_PARKED, (uint32_t)MDBX_TID_TXN_OUSTED);
#endif
          if (likely(ousted)) {
            ousted = safe64_reset_compare(&slot->txnid, slot_snap_mvcc);
            NOTICE("ousted-%s parked read-txn %" PRIaTXN ", pid %zu, tid 0x%" PRIx64, ousted ? "complete" : "half",
                   slot_snap_mvcc, (size_t)pid, tid);
            eASSERT0(env, ousted || safe64_read(&slot->txnid) > orsi.oldest_txnid);
            continue;
          }
          goto retry;
        }
        stucked_tid = tid;
        stucked_retired = atomic_load64(&lck->rdt[i].snapshot_pages_retired, mo_Relaxed);
        stucked = slot;
      }
    }

    if (!stucked)
      break;

    mdbx_pid_t pid = atomic_load_pid(&stucked->pid, mo_AcquireRelease);
    if (safe64_read(&stucked->txnid) != straggler || safe64_read(&stucked->tid) != stucked_tid || !pid)
      continue;

    if (optional_obstacle) {
      optional_obstacle->pid = pid;
      optional_obstacle->tid = (mdbx_tid_t)((intptr_t)stucked_tid);
      optional_obstacle->txnid = straggler;
    }

    if (!callback)
      break;

    const meta_ptr_t recent = meta_recent(env, &env->txn->wr.troika);
    const txnid_t lag = (recent.txnid - straggler) / xMDBX_TXNID_STEP;
    const uint64_t recent_retired = unaligned_peek_u64(4, recent.ptr_c->pages_retired);
    const size_t space =
        (recent_retired > stucked_retired) ? pgno2bytes(env, (pgno_t)(recent_retired - stucked_retired)) : 0;
    int rc = callback(env, env->txn, pid, (mdbx_tid_t)((intptr_t)stucked_tid), straggler,
                      (lag < UINT_MAX) ? (unsigned)lag : UINT_MAX, space, retry);
    if (rc < 0) {
      /* hsr returned error and/or agree MDBX_MAP_FULL error */
      break;
    }

    if (rc > 0) {
      if (rc == 1) {
        /* hsr reported transaction (will be) aborted asynchronous */
        safe64_reset_compare(&stucked->txnid, straggler);
      } else {
        /* hsr reported reader process was killed and slot should be cleared */
        safe64_reset(&stucked->txnid, true);
        atomic_store64(&stucked->tid, MDBX_TID_TXN_OUSTED, mo_Relaxed);
        atomic_store32(&stucked->pid, 0, mo_AcquireRelease);
      }
    } else if (!notify_eof_of_loop) {
#if MDBX_ENABLE_PROFGC
      env->lck->pgops.gc_prof.kicks += 1;
#endif /* MDBX_ENABLE_PROFGC */
      notify_eof_of_loop = true;
    }

  } while (++retry < INT_MAX);

  if (notify_eof_of_loop) {
    /* notify end of hsr-loop */
    const txnid_t turn = orsi.oldest_txnid - straggler;
    if (turn)
      NOTICE("hsr-kick: done turn %" PRIaTXN " -> %" PRIaTXN " +%" PRIaTXN, straggler, orsi.oldest_txnid, turn);
    callback(env, env->txn, 0, 0, straggler, (turn < UINT_MAX) ? (unsigned)turn : UINT_MAX, 0, -retry);
  }
  return orsi.oldest_txnid > straggler;
}
