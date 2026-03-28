/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

static inline mdbx_pid_t ro_slot_pid(const reader_slot_t *slot) { return slot->pid.weak; }

static inline uint64_t ro_slot_tid(const reader_slot_t *slot) { return slot->tid.weak; }

static inline txnid_t ro_slot_txnid(const reader_slot_t *slot) { return slot->txnid.weak; }

MDBX_MAYBE_UNUSED static inline bool is_ro_txn(MDBX_txn *txn) {
  return (txn->flags & (txn_ro_flat | MDBX_TXN_DIRTY | MDBX_TXN_SPILLS | MDBX_TXN_HAS_CHILD)) == txn_ro_flat &&
         !txn->parent;
}

static __noinline int ro_slot_mismatch_pid(MDBX_txn *txn, const mdbx_pid_t snap_slot_pid) {
  mdbx_pid_t current_pid = osal_getpid();
  if (current_pid == txn->env->registered_reader_pid)
    ERROR("readonly-transaction %p mvcc %" PRIu64 " was unexpectedly ousted (expectd pid %zu, seen %zu)",
          __Wpedantic_format_voidptr(txn), txn->txnid, (size_t)current_pid, (size_t)snap_slot_pid);
  else
    NOTICE("ignore pid mismatch of transaction %p mvcc %" PRIu64 "reader-slot after fork",
           __Wpedantic_format_voidptr(txn), txn->txnid);

  return (current_pid != snap_slot_pid) ? MDBX_BAD_RSLOT : MDBX_SUCCESS;
}

static inline int ro_slot_checkpid(MDBX_txn *txn) {
  const mdbx_pid_t snap_slot_pid = ro_slot_pid(txn->ro.slot);
  if (likely(snap_slot_pid == txn->env->registered_reader_pid)) {
    tASSERT(txn, snap_slot_pid == osal_getpid());
    if ((txn->flags & MDBX_TXN_FINISHED) == 0)
      tASSERT(txn, ro_slot_tid(txn->ro.slot) == ((txn->env->flags & MDBX_NOSTICKYTHREADS) ? 0 : osal_thread_self()) ||
                       (ro_slot_tid(txn->ro.slot) >= MDBX_TID_TXN_OUSTED &&
                        (txn->flags & (MDBX_TXN_PARKED | MDBX_TXN_OUSTED))));
    return MDBX_SUCCESS;
  }
  return ro_slot_mismatch_pid(txn, snap_slot_pid);
}

static int ro_slot_alloc(MDBX_txn *txn) {
  tASSERT(txn, is_ro_txn(txn) && !txn->ro.slot);
  MDBX_env *const env = txn->env;
  if (unlikely(!env->lck_mmap.lck))
    return MDBX_SUCCESS;

  if (env->flags & ENV_TXKEY) {
    eASSERT(env, !(env->flags & MDBX_NOSTICKYTHREADS));
    reader_slot_t *slot = thread_rthc_get(env->me_txkey);
    if (likely(slot)) {
      const mdbx_pid_t snap_slot_pid = ro_slot_pid(slot);
      tASSERT(txn, env->registered_reader_pid == osal_getpid());
      if (likely(snap_slot_pid == env->registered_reader_pid && ro_slot_txnid(slot) >= SAFE64_INVALID_THRESHOLD)) {
        tASSERT(txn, ro_slot_tid(slot) == ((env->flags & MDBX_NOSTICKYTHREADS) ? 0 : osal_thread_self()));
        txn->ro.slot = slot;
        return MDBX_SUCCESS;
      }
      if (unlikely(snap_slot_pid) || !(globals.runtime_flags & MDBX_DBG_LEGACY_MULTIOPEN))
        return MDBX_BAD_RSLOT;
      thread_rthc_set(env->me_txkey, nullptr);
    }
  } else {
    eASSERT(env, (env->flags & MDBX_NOSTICKYTHREADS));
  }

  bsr_t brs = mvcc_bind_slot(env);
  if (likely(brs.err == MDBX_SUCCESS)) {
    tASSERT(txn, ro_slot_pid(brs.slot) == osal_getpid());
    tASSERT(txn, ro_slot_tid(brs.slot) == ((env->flags & MDBX_NOSTICKYTHREADS) ? 0 : osal_thread_self()));
  }

  txn->ro.slot = brs.slot;
  return brs.err;
}

static void ro_slot_release(MDBX_txn *txn) {
  tASSERT(txn, is_ro_txn(txn) && txn->ro.slot);
  reader_slot_t *slot = txn->ro.slot;
  MDBX_env *const env = txn->env;
  eASSERT(env, ro_slot_pid(slot) == osal_getpid());
  eASSERT(env, slot->txnid.weak >= SAFE64_INVALID_THRESHOLD);
  if (ro_slot_pid(slot) == osal_getpid()) {
    safe64_reset(&slot->txnid, false);
    if ((env->flags & ENV_TXKEY) == 0)
      atomic_store32(&slot->pid, 0, mo_Relaxed);
  }
  txn->ro.slot = nullptr;
}

static inline int ro_slot_get(MDBX_txn *txn) {
  reader_slot_t *slot = txn->ro.slot;
  STATIC_ASSERT(sizeof(uintptr_t) <= sizeof(slot->tid));
  if (likely(slot))
    return ro_slot_checkpid(txn);
  return ro_slot_alloc(txn);
}

static int ro_slot_clean(MDBX_txn *txn) {
  reader_slot_t *const slot = txn->ro.slot;
  if (unlikely(!slot))
    return MDBX_SUCCESS;

  MDBX_env *const env = txn->env;
  if (unlikely(!env->lck)) {
    txn->ro.slot = nullptr;
    return MDBX_SUCCESS;
  }

  int rc = ro_slot_checkpid(txn);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  if (likely((txn->flags & MDBX_TXN_FINISHED) == 0)) {
    if (likely((txn->flags & MDBX_TXN_PARKED) == 0)) {
      ENSURE_OBJ(env, txn->txnid >= /* paranoia is appropriate here */ env->lck->cached_oldest_txnid.weak);
      eASSERT(env, txn->txnid == slot->txnid.weak && slot->txnid.weak >= env->lck->cached_oldest_txnid.weak);
    } else {
      txn->flags -= MDBX_TXN_PARKED;
      if (safe64_read(&slot->tid) == MDBX_TID_TXN_OUSTED)
        txn->flags |= MDBX_TXN_OUSTED;
      do {
        safe64_reset(&slot->txnid, false);
        atomic_store64(&slot->tid, txn->owner, mo_AcquireRelease);
        atomic_yield();
      } while (unlikely(safe64_read(&slot->txnid) < SAFE64_INVALID_THRESHOLD || safe64_read(&slot->tid) != txn->owner));
    }
    dxb_sanitize_tail(env, nullptr);
    atomic_store32(&slot->snapshot_pages_used, 0, mo_Relaxed);
    safe64_reset(&slot->txnid, true);
    atomic_store32(&env->lck->rdt_refresh_flag, true, mo_Relaxed);
  } else {
    ENSURE_OBJ(env, safe64_read(&slot->txnid) >= /* paranoia is appropriate here */ SAFE64_INVALID_THRESHOLD);
  }
  return MDBX_SUCCESS;
}

static int ro_seize(MDBX_txn *txn) {
  /* Seek & fetch the last meta */
  troika_t troika = meta_tap(txn->env);
  uint64_t timestamp = 0;
  size_t loop = 0;
  txn->flags &= ~MDBX_TXN_FINISHED;
  do {
    MDBX_env *const env = txn->env;
    const meta_ptr_t head = likely(env->stuck_meta < 0) ? /* regular */ meta_recent(env, &troika)
                                                        : /* recovery mode */ meta_ptr(env, env->stuck_meta);
    reader_slot_t *const slot = txn->ro.slot;
    if (likely(slot != nullptr)) {
      safe64_reset(&slot->txnid, true);
      atomic_store32(&slot->snapshot_pages_used, head.ptr_v->geometry.first_unallocated, mo_Relaxed);
      atomic_store64(&slot->snapshot_pages_retired, unaligned_peek_u64_volatile(4, head.ptr_v->pages_retired),
                     mo_Relaxed);
      safe64_write(&slot->txnid, head.txnid);
      eASSERT(env, ro_slot_pid(slot) == osal_getpid());
      eASSERT(env, ro_slot_tid(slot) == ((env->flags & MDBX_NOSTICKYTHREADS) ? 0 : osal_thread_self()));
      eASSERT(env, ro_slot_txnid(slot) == head.txnid || (ro_slot_txnid(slot) >= SAFE64_INVALID_THRESHOLD &&
                                                         head.txnid < env->lck->cached_oldest_txnid.weak));
      atomic_store32(&env->lck->rdt_refresh_flag, true, mo_AcquireRelease);
    } else {
      /* exclusive mode without lck */
      eASSERT(env, !env->lck_mmap.lck && env->lck == lckless_stub(env));
    }
    jitter4testing(true);

    if (unlikely(meta_should_retry(env, &troika))) {
      timestamp = 0;
      continue;
    }

    /* Snap the state from current meta-head */
    int err = coherency_fetch_head(txn, head, &timestamp);
    jitter4testing(false);
    if (unlikely(err != MDBX_SUCCESS)) {
      if (err != MDBX_RESULT_TRUE)
        return err;
      continue;
    }

    const uint64_t snap_oldest = atomic_load64(&env->lck->cached_oldest_txnid, mo_AcquireRelease);
    if (unlikely(txn->txnid < snap_oldest)) {
      if (env->stuck_meta >= 0) {
        ERROR("target meta-page %i is referenced to an obsolete MVCC-snapshot "
              "%" PRIaTXN " < cached-oldest %" PRIaTXN,
              env->stuck_meta, txn->txnid, snap_oldest);
        return MDBX_MVCC_RETARDED;
      }
      continue;
    }

    if (!slot || likely(txn->txnid == atomic_load64(&slot->txnid, mo_Relaxed)))
      return MDBX_SUCCESS;

  } while (likely(++loop < 42));

  ERROR("bailout waiting for valid snapshot (%s)", "meta-pages are too volatile");
  return MDBX_PROBLEM;
}

static int ro_start_continue(MDBX_txn *txn) {
  MDBX_env *const env = txn->env;
  reader_slot_t *slot = txn->ro.slot;

  txn->owner =
      likely(slot) ? (uintptr_t)ro_slot_tid(slot) : ((env->flags & MDBX_NOSTICKYTHREADS) ? 0 : osal_thread_self());

  if ((env->flags & MDBX_NOSTICKYTHREADS) == 0 && env->txn && unlikely(env->basal_txn->owner == txn->owner) &&
      (globals.runtime_flags & MDBX_DBG_LEGACY_OVERLAP) == 0)
    return MDBX_TXN_OVERLAPPING;

  int err = ro_seize(txn);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  err = txn_setup_primal(txn);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  return MDBX_SUCCESS;
}

int txn_ro_start(MDBX_txn *txn, bool prepare_only) {
  MDBX_env *const env = txn->env;
  txn->flags = txn_ro_flat | MDBX_TXN_FINISHED | (env->flags & (MDBX_WRITEMAP | MDBX_NOSTICKYTHREADS));
  int err = ro_slot_get(txn);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  if (prepare_only) {
    eASSERT(env, txn->txnid == 0);
    eASSERT(env, txn->owner == 0);
    eASSERT(env, txn->n_dbi == 0);
    reader_slot_t *slot = txn->ro.slot;
    if (likely(slot)) {
      eASSERT(env, slot->snapshot_pages_used.weak == 0);
      eASSERT(env, slot->txnid.weak >= SAFE64_INVALID_THRESHOLD);
      atomic_store32(&slot->snapshot_pages_used, 0, mo_Relaxed);
    }
    txn->flags = txn_ro_flat | MDBX_TXN_FINISHED;
    return MDBX_SUCCESS;
  }

  err = ro_start_continue(txn);
  if (unlikely(err != MDBX_SUCCESS)) {
    txn_ro_reset(txn);
    return err;
  }

  eASSERT(env, pgno2bytes(env, txn->geo.first_unallocated) <= env->dxb_mmap.current);
  eASSERT(env, env->dxb_mmap.limit >= env->dxb_mmap.current);
#if defined(_WIN32) || defined(_WIN64)
  const size_t used_bytes = pgno2bytes(env, txn->geo.first_unallocated);
  if (((used_bytes > env->geo_in_bytes.lower && env->geo_in_bytes.shrink) ||
       (globals.running_under_Wine &&
        /* under Wine acquisition of remap_lock is always required,
         * since Wine don't support section extending,
         * i.e. in both cases unmap+map are required. */
        used_bytes < env->geo_in_bytes.upper && env->geo_in_bytes.grow)) &&
      /* avoid recursive use SRW */ (txn->flags & MDBX_NOSTICKYTHREADS) == 0) {
    imports.srwl_AcquireShared(&env->remap_lock);
    txn->flags |= txn_shrink_allowed;
  }
#endif /* Windows */

  dxb_sanitize_tail(env, txn);
  ENSURE_OBJ(env, txn->txnid >=
                      /* paranoia is appropriate here */ env->lck->cached_oldest_txnid.weak);
  tASSERT(txn, txn->dbs[FREE_DBI].flags == MDBX_INTEGERKEY);
  tASSERT(txn, check_table_flags(txn->dbs[MAIN_DBI].flags));
  return MDBX_SUCCESS;
}

int txn_ro_reset(MDBX_txn *txn) {
  DEBUG("%s txn %" PRIaTXN "%c-0x%X %p  on env %p, root page %" PRIaPGNO "/%" PRIaPGNO, "reset", txn->txnid, 'r',
        txn->flags, __Wpedantic_format_voidptr(txn), __Wpedantic_format_voidptr(txn->env), txn->dbs[MAIN_DBI].root,
        txn->dbs[FREE_DBI].root);

  tASSERT(txn, is_ro_txn(txn));
  if (txn->flags & txn_may_have_cursors)
    txn_done_cursors(txn);

  tASSERT(txn, (txn->flags & txn_may_have_cursors) == 0);
  txn->n_dbi = 0; /* prevent further DBI activity */

#if defined(_WIN32) || defined(_WIN64)
  if ((txn->flags & (MDBX_TXN_FINISHED | txn_shrink_allowed)) == txn_shrink_allowed)
    imports.srwl_ReleaseShared(&txn->env->remap_lock);
#endif /* Windows */

  int rc = ro_slot_clean(txn);
  txn->flags |= MDBX_TXN_FINISHED;
  txn->owner = 0;
  txn->txnid = INVALID_TXNID;
  return rc;
}

void txn_ro_free(MDBX_txn *txn) {
  tASSERT(txn, is_ro_txn(txn));
  if (txn->ro.slot) {
    txn_ro_reset(txn);
    ro_slot_release(txn);
  }

  tASSERT(txn, (txn->flags & txn_may_have_cursors) == 0);
  txn->signature = 0;
  osal_free(txn);
}

int txn_ro_park(MDBX_txn *txn, bool autounpark) {
  reader_slot_t *const slot = txn->ro.slot;
  tASSERT(txn, (txn->flags & (MDBX_TXN_FINISHED | txn_ro_flat | MDBX_TXN_PARKED)) == txn_ro_flat);
  tASSERT(txn, ro_slot_tid(slot) < MDBX_TID_TXN_OUSTED);
  if (unlikely((txn->flags & (MDBX_TXN_FINISHED | txn_ro_flat | MDBX_TXN_PARKED)) != txn_ro_flat))
    return MDBX_BAD_TXN;

  const mdbx_pid_t pid = atomic_load_pid(&slot->pid, mo_Relaxed);
  const uint64_t tid = atomic_load64(&slot->tid, mo_Relaxed);
  const txnid_t txnid = atomic_load64(&slot->txnid, mo_Relaxed);
  if (unlikely(pid != txn->env->pid)) {
    ERROR("unexpected pid %zu%s%zu", (size_t)pid, " != must ", (size_t)txn->env->pid);
    return MDBX_PROBLEM;
  }
  if (unlikely(tid != txn->owner || txnid != txn->txnid)) {
    ERROR("unexpected thread-id 0x%" PRIx64 "%s0x%0zx"
          " and/or txn-id %" PRIaTXN "%s%" PRIaTXN,
          tid, " != must ", txn->owner, txnid, " != must ", txn->txnid);
    return MDBX_BAD_RSLOT;
  }

  if (unlikely((txn->flags & MDBX_TXN_ERROR)))
    return ro_slot_clean(txn);

  atomic_store64(&slot->tid, MDBX_TID_TXN_PARKED, mo_AcquireRelease);
  atomic_store32(&txn->env->lck->rdt_refresh_flag, true, mo_Relaxed);
  txn->flags += autounpark ? MDBX_TXN_PARKED | MDBX_TXN_AUTOUNPARK : MDBX_TXN_PARKED;
  return MDBX_SUCCESS;
}

int txn_ro_unpark(MDBX_txn *txn) {
  if (unlikely((txn->flags & (MDBX_TXN_FINISHED | MDBX_TXN_HAS_CHILD | txn_ro_flat | MDBX_TXN_PARKED)) !=
               (txn_ro_flat | MDBX_TXN_PARKED)))
    return (txn->flags & MDBX_TXN_OUSTED) ? MDBX_OUSTED : MDBX_BAD_TXN;

  for (reader_slot_t *const slot = txn->ro.slot; slot; atomic_yield()) {
    const mdbx_pid_t pid = atomic_load_pid(&slot->pid, mo_Relaxed);
    uint64_t tid = safe64_read(&slot->tid);
    uint64_t txnid = safe64_read(&slot->txnid);
    if (unlikely(pid != txn->env->pid)) {
      ERROR("unexpected pid %zu%s%zu", (size_t)pid, " != expected ", (size_t)txn->env->pid);
      return MDBX_PROBLEM;
    }
    if (unlikely(tid == MDBX_TID_TXN_OUSTED || txnid >= SAFE64_INVALID_THRESHOLD))
      break;
    if (unlikely(tid != MDBX_TID_TXN_PARKED || txnid != txn->txnid)) {
      ERROR("unexpected thread-id 0x%" PRIx64 "%s0x%" PRIx64 " and/or txn-id %" PRIaTXN "%s%" PRIaTXN, tid, " != must ",
            MDBX_TID_TXN_OUSTED, txnid, " != must ", txn->txnid);
      break;
    }
    if (unlikely((txn->flags & MDBX_TXN_ERROR)))
      break;

#if MDBX_64BIT_CAS
    if (unlikely(!atomic_cas64(&slot->tid, MDBX_TID_TXN_PARKED, txn->owner)))
      continue;
#else
    atomic_store32(&slot->tid.high, (uint32_t)((uint64_t)txn->owner >> 32), mo_Relaxed);
    if (unlikely(!atomic_cas32(&slot->tid.low, (uint32_t)MDBX_TID_TXN_PARKED, (uint32_t)txn->owner))) {
      atomic_store32(&slot->tid.high, (uint32_t)(MDBX_TID_TXN_PARKED >> 32), mo_AcquireRelease);
      continue;
    }
#endif
    txnid = safe64_read(&slot->txnid);
    tid = safe64_read(&slot->tid);
    if (unlikely(txnid != txn->txnid || tid != txn->owner)) {
      ERROR("unexpected thread-id 0x%" PRIx64 "%s0x%zx"
            " and/or txn-id %" PRIaTXN "%s%" PRIaTXN,
            tid, " != must ", txn->owner, txnid, " != must ", txn->txnid);
      break;
    }
    txn->flags &= ~(MDBX_TXN_PARKED | MDBX_TXN_AUTOUNPARK);
    return MDBX_SUCCESS;
  }

  int err = txn_ro_reset(txn);
  return (err == MDBX_SUCCESS && (txn->flags & MDBX_TXN_OUSTED)) ? MDBX_OUSTED : err;
}

int txn_ro_clone(const MDBX_txn *const origin, MDBX_txn *const clone) {
  int err = ro_slot_get(clone);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  if (unlikely((origin->flags & MDBX_TXN_FINISHED) || origin->txnid < MIN_TXNID || origin->txnid > MAX_TXNID))
    return MDBX_BAD_TXN;

  clone->flags = (origin->flags & (MDBX_NOSTICKYTHREADS | MDBX_WRITEMAP | MDBX_TXN_AUTOUNPARK | MDBX_TXN_PARKED)) |
                 txn_ro_flat | MDBX_TXN_FINISHED;
  clone->owner = likely(clone->ro.slot) ? (uintptr_t)ro_slot_tid(clone->ro.slot)
                                        : ((clone->flags & MDBX_NOSTICKYTHREADS) ? 0 : osal_thread_self());
  clone->txnid = txn_basis_snapshot(origin);
  clone->front_txnid = clone->txnid;

  MDBX_env *const env = origin->env;
  if (origin->flags & txn_ro_flat) {
    if ((clone->flags & MDBX_NOSTICKYTHREADS) == 0 && env->txn && unlikely(env->basal_txn->owner == clone->owner) &&
        (globals.runtime_flags & MDBX_DBG_LEGACY_OVERLAP) == 0)
      return MDBX_TXN_OVERLAPPING;

    if (likely(clone->ro.slot)) {
      const uint32_t pages_used = origin->ro.slot ? origin->ro.slot->snapshot_pages_used.weak : 0;
      const uint64_t pages_retired = origin->ro.slot ? origin->ro.slot->snapshot_pages_retired.weak : 0;
      atomic_store32(&clone->ro.slot->snapshot_pages_used, pages_used, mo_Relaxed);
      atomic_store64(&clone->ro.slot->snapshot_pages_retired, pages_retired, mo_Relaxed);
      safe64_write(&clone->ro.slot->txnid, (clone->flags & MDBX_TXN_PARKED) ? MDBX_TID_TXN_PARKED : clone->txnid);
    } else {
      /* exclusive mode without lck */
      eASSERT(env, !env->lck_mmap.lck && env->lck == lckless_stub(env));
    }

    /* Setup db info */
    clone->geo = origin->geo;
    clone->canary = origin->canary;
    memcpy(clone->dbs, origin->dbs, sizeof(clone->dbs[0]) * CORE_DBS);
    tASSERT(clone, clone->dbs[FREE_DBI].flags == MDBX_INTEGERKEY);
    tASSERT(clone, check_table_flags(clone->dbs[MAIN_DBI].flags));
    VALGRIND_MAKE_MEM_UNDEFINED(clone->dbi_state, env->max_dbi);
#if MDBX_ENABLE_DBI_SPARSE
    clone->n_dbi = CORE_DBS;
    VALGRIND_MAKE_MEM_UNDEFINED(clone->dbi_sparse,
                                ceil_powerof2(env->max_dbi, CHAR_BIT * sizeof(clone->dbi_sparse[0])) / CHAR_BIT);
    clone->dbi_sparse[0] = (1u << CORE_DBS) - 1;
#else
    clone->n_dbi = (env->n_dbi < 8) ? env->n_dbi : 8;
    if (clone->n_dbi > CORE_DBS)
      memset(clone->dbi_state + CORE_DBS, 0, clone->n_dbi - CORE_DBS);
#endif /* MDBX_ENABLE_DBI_SPARSE */
    clone->dbi_state[FREE_DBI] = DBI_LINDO | DBI_VALID;
    clone->dbi_state[MAIN_DBI] = DBI_LINDO | DBI_VALID;
    clone->cursors[FREE_DBI] = nullptr;
    clone->cursors[MAIN_DBI] = nullptr;
    clone->dbi_seqs[FREE_DBI] = origin->dbi_seqs[FREE_DBI];
    clone->dbi_seqs[MAIN_DBI] = origin->dbi_seqs[MAIN_DBI];

    if (!(clone->flags & MDBX_TXN_PARKED)) {
      if (likely(clone->ro.slot) && unlikely(clone->txnid != atomic_load64(&clone->ro.slot->txnid, mo_Relaxed)))
        return MDBX_OUSTED;
      if (unlikely(clone->txnid < atomic_load64(&env->lck->cached_oldest_txnid, mo_AcquireRelease)))
        return MDBX_MVCC_RETARDED;
    }
    return MDBX_SUCCESS;
  }

  return ro_seize(clone);
}
