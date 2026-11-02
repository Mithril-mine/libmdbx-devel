/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file dxb-resize.c — datafile/mapping resize and readahead control.

#include "internals.h"

#if IS_WINDOWS
/* Acquire the exclusive remap-lock for a resize that requires un-mapping the
 * mapping (a shrink, or a grow beyond the current mapping limit, or an
 * explicit geometry change) on Windows.
 *
 * A reader may hold the lock shared for an unbounded time, so we must not wait
 * indefinitely — that is exactly the #154 deadlock. A few bounded attempts
 * with a time-slice yield in between let a reader that is about to finish
 * release the lock, while a long-lived reader still defers the operation
 * promptly. This is a plain static inline so it can later be turned into a
 * user-overridable callback.
 *
 * Note: remap_lock is a process-local SRWL (a field of MDBX_env, not of the
 * shared lock file), so failing to acquire it is by itself the precise test
 * for "an in-process reader holds it shared" — no separate reader-table scan
 * is needed to decide whether to defer.
 *
 * On failure the resize is deferred and the return code is chosen per the
 * resize mode (issue #154):
 *  - implicit_shrink → MDBX_EPERM, which dxb_sync_locked() and the nested
 *    rollback path in txn-nested.c treat as "regular on Windows": the commit
 *    still succeeds and the shrink is re-attempted later, since the shrink
 *    predicate is based on the real file size (see dxb_shrink_maybe() and
 *    dxb_shrink_bottom()).
 *  - implicit_grow (the file was grown by another process beyond this
 *    process's mapping limit, so a remap is needed) and explicit_resize →
 *    MDBX_UNABLE_EXTEND_MAPSIZE, the documented code for "the requested
 *    mapping size could not be applied right now"; it is returned to the
 *    caller of mdbx_txn_begin()/mdbx_env_set_geometry() without poisoning the
 *    env, and the operation may be retried once the readers are gone (or the
 *    environment is reopened).
 * The message is a WARNING, not an ERROR: this is an expected, recoverable
 * condition while a read transaction is open. */
static int windows_remap_lock_exclusive(MDBX_env *env, const enum resize_mode mode) {
  for (int attempt = 0; attempt < 5; ++attempt) {
    if (imports.srwl_TryAcquireExclusive(&env->remap_lock))
      return MDBX_SUCCESS;
    if (attempt + 1 < 5)
      osal_yield(); /* give a finishing reader a chance to release the lock */
  }
  WARNING("%s", "unable to acquire the exclusive remap-lock for a database shrink/remap: "
                "another reader holds it shared, cannot unmap and remap safely");
  /* an implicit shrink is deferred silently by the commit path, everything else
   * is reported as "the mapping size could not be applied right now" */
  return (mode == implicit_shrink) ? MDBX_EPERM : MDBX_UNABLE_EXTEND_MAPSIZE;
}
#endif /* Windows */

__cold int dxb_resize(MDBX_env *const env, const pgno_t allocated_pgno, const pgno_t size_pgno, pgno_t limit_pgno,
                      const enum resize_mode mode) {
  /* Acquire guard to avoid collision between read and write txns
   * around geo_in_bytes and dxb_mmap */
#if IS_WINDOWS
  /* resize_mutex (a leaf) serializes resizers among themselves. Since a plain
   * grow now holds only the SHARED remap-lock, two concurrent growers — a read
   * txn noticing the file grew by another process, and the writer growing it —
   * would otherwise race on the env->dxb_mmap bookkeeping and call
   * NtExtendSection() on the same section simultaneously. Readers never take
   * resize_mutex and the exclusive remap-lock is only ever try-acquired while
   * holding it, so the shared acquire below cannot block behind another
   * resizer: no deadlock cycle exists. Lock order is always
   * resize_mutex → remap_lock, released in reverse. */
  int rc = osal_fastmutex_acquire(&env->resize_mutex);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  /* Whether the mapping can be extended in place (NtExtendSection: no unmap,
   * no address change): only a plain grow whose new size still fits the
   * current mapping limit AND the platform supports section extending (not
   * under Wine, where NtExtendSection is unavailable and every resize needs
   * unmap+remap). Such a grow needs no exclusive remap-lock and no thread
   * suspension, so it cannot deadlock with long-lived readers (issue #154).
   * A grow beyond the limit, a shrink, or an explicit resize all need the
   * mapping to be unmapped and remapped, hence the exclusive remap-lock. */
  const bool no_remap_needed =
      (mode == implicit_grow) && imports.NtExtendSection && pgno_ceil2os_bytes(env, size_pgno) <= env->dxb_mmap.limit;
  bool have_exclusive = false;
  if (no_remap_needed) {
    imports.srwl_AcquireShared(&env->remap_lock);
  } else {
    /* NOTE: deliberately no fallback to the blocking srwl_AcquireExclusive()
     * here — that is exactly the #154 deadlock, since a reader may hold the
     * lock shared for an unbounded time. Deferring is always safe. */
    rc = windows_remap_lock_exclusive(env, mode);
    if (unlikely(rc != MDBX_SUCCESS)) {
      osal_fastmutex_release(&env->resize_mutex);
      return rc;
    }
    have_exclusive = true;
  }
  mdbx_handle_array_t *suspended = nullptr;
  mdbx_handle_array_t array_onstack;
#else
  bool should_unlock_rdt = false;
  int rc = osal_fastmutex_acquire(&env->remap_lock);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;
#endif

  const size_t prev_size = env->dxb_mmap.current;
  const size_t prev_limit = env->dxb_mmap.limit;
  const pgno_t prev_limit_pgno = bytes2pgno(env, prev_limit);
  eASSERT0(env, limit_pgno >= size_pgno);
  eASSERT0(env, size_pgno >= allocated_pgno);
  if (mode < explicit_resize && size_pgno <= prev_limit_pgno) {
    /* The actual mapsize may be less since the geo.upper may be changed
     * by other process. Avoids remapping until it necessary. */
    limit_pgno = prev_limit_pgno;
  }
  const size_t limit_bytes = pgno_ceil2os_bytes(env, limit_pgno);
  const size_t size_bytes = pgno_ceil2os_bytes(env, size_pgno);
  const void *const prev_map = env->dxb_mmap.base;

  VERBOSE("resize(env-flags 0x%x, mode %d) datafile/mapping: "
          "present %" PRIuPTR " -> %" PRIuPTR ", "
          "limit %" PRIuPTR " -> %" PRIuPTR,
          env->flags, mode, prev_size, size_bytes, prev_limit, limit_bytes);

  eASSERT0(env, limit_bytes >= size_bytes);
  eASSERT0(env, bytes2pgno(env, size_bytes) >= size_pgno);
  eASSERT0(env, bytes2pgno(env, limit_bytes) >= limit_pgno);

  unsigned mresize_flags = env->flags & (MDBX_RDONLY | MDBX_WRITEMAP | MDBX_UTTERLY_NOSYNC);
  if (mode >= implicit_shrink)
    mresize_flags |= txn_unmap_allowed;

  if (limit_bytes == env->dxb_mmap.limit && size_bytes == env->dxb_mmap.current && size_bytes == env->dxb_mmap.filesize)
    goto bailout;

  /* При использовании MDBX_NOSTICKYTHREADS с транзакциями могут работать любые
   * потоки и у нас нет информации о том, какие именно. Поэтому нет возможности
   * выполнить remap-действия требующие приостановки работающих с БД потоков. */
  if ((env->flags & MDBX_NOSTICKYTHREADS) == 0) {
#if IS_WINDOWS
    if (have_exclusive &&
        ((size_bytes < env->dxb_mmap.current && mode > implicit_grow) || limit_bytes != env->dxb_mmap.limit)) {
      /* Local threads must be suspended whenever the mapping is UNMAPPED and
       * re-created (a shrink, or a grow past the mapping limit), because the
       * base address may change and any thread touching the old view would
       * fault. Windows allows extending a read-write section in place, but NOT
       * a corresponding mapped view, so a remap is the only way to grow past the
       * limit or to shrink.
       *
       * NOTE (issue #154): a PLAIN in-place grow (NtExtendSection within the
       * current limit) deliberately does NOT come here and does NOT suspend
       * threads — it takes the remap-lock only SHARED so it cannot deadlock
       * with a long-lived reader. It keeps the base address and the existing
       * view valid, so concurrent readers of this process may stay active while
       * the section is extended.
       *
       * REMINDER / WARNING (keep as bread crumbs): at least on Windows 10 1803
       * the whole mapped section was reportedly unavailable for a short time
       * during NtExtendSection()/VirtualAlloc(), and under Wine a section
       * extending is not supported at all (hence `no_remap_needed` requires
       * imports.NtExtendSection). The in-place grow without suspension was
       * validated on the Windows CI cells and native multi-reader runs; if a
       * crash ever appears while extending, this historical caveat is the first
       * thing to re-examine. */
      array_onstack.limit = ARRAY_LENGTH(array_onstack.handles);
      array_onstack.count = 0;
      suspended = &array_onstack;
      rc = osal_suspend_threads_before_remap(env, &suspended);
      if (rc != MDBX_SUCCESS) {
        ERROR("failed suspend-for-remap: errcode %d", rc);
        goto bailout;
      }
      mresize_flags |=
          (mode < explicit_resize) ? MDBX_MRESIZE_MAY_UNMAP : MDBX_MRESIZE_MAY_UNMAP | MDBX_MRESIZE_MAY_MOVE;
    }
#else  /* Windows */
    lck_t *const lck = env->lck_mmap.lck;
    if (mode == explicit_resize && limit_bytes != env->dxb_mmap.limit) {
      mresize_flags |= MDBX_MRESIZE_MAY_UNMAP | MDBX_MRESIZE_MAY_MOVE;
      if (lck) {
        int err = lck_rdt_lock(env) /* lock readers table until remap done */;
        if (unlikely(MDBX_IS_ERROR(err))) {
          rc = err;
          goto bailout;
        }
        should_unlock_rdt = true;

        /* looking for readers from this process */
        const size_t snap_nreaders = rdt_count(env, lck);
        eASSERT0(env, mode == explicit_resize);
        for (size_t i = 0; i < snap_nreaders; ++i) {
          if (lck->rdt[i].pid.weak == (size_t)env->registered_reader_pid &&
              lck->rdt[i].tid.weak != osal_thread_self() && lck->rdt[i].tid.weak < MDBX_TID_TXN_OUSTED) {
            /* the base address of the mapping can't be changed since
             * the other reader thread from this process exists. */
            lck_rdt_unlock(env);
            should_unlock_rdt = false;
            mresize_flags &= ~(MDBX_MRESIZE_MAY_UNMAP | MDBX_MRESIZE_MAY_MOVE);
            break;
          }
        }
      }
    }
#endif /* ! Windows */
  }

  const pgno_t aligned_munlock_pgno =
      (mresize_flags & (MDBX_MRESIZE_MAY_UNMAP | MDBX_MRESIZE_MAY_MOVE)) ? 0 : bytes2pgno(env, size_bytes);
  if (mresize_flags & (MDBX_MRESIZE_MAY_UNMAP | MDBX_MRESIZE_MAY_MOVE)) {
    env_clear_incore_cache(env);
    if ((env->flags & MDBX_WRITEMAP) && env->lck->unsynced_pages.weak) {
      rc = dxb_msync(env, allocated_pgno, MDBX_SYNC_KICK);
      if (unlikely(rc != MDBX_SUCCESS))
        goto bailout;
    }
  }
  munlock_after(env, aligned_munlock_pgno, size_bytes);

  if (size_bytes < prev_size && mode > implicit_grow) {
    NOTICE("resize-MADV_%s %u..%u", (env->flags & MDBX_WRITEMAP) ? "REMOVE" : "DONTNEED", size_pgno,
           bytes2pgno(env, prev_size));
    const uint32_t munlocks_before = atomic_load32(&env->lck->mlcnt[1], mo_Relaxed);
    rc = MDBX_RESULT_TRUE;
#if defined(MADV_REMOVE)
    if (env->flags & MDBX_WRITEMAP)
      rc = madvise(ptr_disp(env->dxb_mmap.base, size_bytes), prev_size - size_bytes, MADV_REMOVE)
               ? ignore_enosys_and_eagain(errno)
               : MDBX_SUCCESS;
#endif /* MADV_REMOVE */
#if defined(MADV_DONTNEED)
    if (rc == MDBX_RESULT_TRUE)
      rc = madvise(ptr_disp(env->dxb_mmap.base, size_bytes), prev_size - size_bytes, MADV_DONTNEED)
               ? ignore_enosys_and_eagain(errno)
               : MDBX_SUCCESS;
#elif defined(POSIX_MADV_DONTNEED)
    if (rc == MDBX_RESULT_TRUE)
      rc = ignore_enosys(
          posix_madvise(ptr_disp(env->dxb_mmap.base, size_bytes), prev_size - size_bytes, POSIX_MADV_DONTNEED));
#elif defined(POSIX_FADV_DONTNEED)
    if (rc == MDBX_RESULT_TRUE)
      rc = ignore_enosys(posix_fadvise(env->lazy_fd, size_bytes, prev_size - size_bytes, POSIX_FADV_DONTNEED));
#endif /* MADV_DONTNEED */
    if (unlikely(MDBX_IS_ERROR(rc))) {
      const uint32_t mlocks_after = atomic_load32(&env->lck->mlcnt[0], mo_Relaxed);
      if (rc == MDBX_EINVAL) {
        const int severity = (mlocks_after - munlocks_before) ? MDBX_LOG_NOTICE : MDBX_LOG_WARN;
        if (LOG_ENABLED(severity))
          debug_log(severity, __func__, __LINE__,
                    "%s-madvise: ignore EINVAL (%d) since some pages maybe "
                    "locked (%u/%u mlcnt-processes)",
                    "resize", rc, mlocks_after, munlocks_before);
      } else {
        ERROR("%s-madvise(%s, %zu, +%zu), %u/%u mlcnt-processes, err %d", "mresize", "DONTNEED", size_bytes,
              prev_size - size_bytes, mlocks_after, munlocks_before, rc);
        goto bailout;
      }
    } else
      env->lck->discarded_tail.weak = size_pgno;
  }

  rc = osal_mresize(mresize_flags, &env->dxb_mmap, size_bytes, limit_bytes);
  eASSERT0(env, env->dxb_mmap.limit >= env->dxb_mmap.current);

  if (rc == MDBX_SUCCESS) {
    eASSERT0(env, limit_bytes == env->dxb_mmap.limit);
    eASSERT0(env, size_bytes <= env->dxb_mmap.filesize);
    if (mode == explicit_resize)
      eASSERT0(env, size_bytes == env->dxb_mmap.current);
    else
      eASSERT0(env, size_bytes <= env->dxb_mmap.current);
    env->lck->discarded_tail.weak = size_pgno;
    const bool readahead =
        !(env->flags & MDBX_NORDAHEAD) && mdbx_is_readahead_reasonable(size_bytes, -(intptr_t)prev_size);
    const bool force = limit_bytes != prev_limit || env->dxb_mmap.base != prev_map
#if IS_WINDOWS
                       || prev_size > size_bytes
#endif /* Windows */
        ;
    rc = dxb_set_readahead(env, size_pgno, readahead, force);
  }

bailout:
  if (rc == MDBX_SUCCESS) {
    eASSERT0(env, env->dxb_mmap.limit >= env->dxb_mmap.current);
    eASSERT0(env, limit_bytes == env->dxb_mmap.limit);
    eASSERT0(env, size_bytes <= env->dxb_mmap.filesize);
    if (mode == explicit_resize)
      eASSERT0(env, size_bytes == env->dxb_mmap.current);
    else
      eASSERT0(env, size_bytes <= env->dxb_mmap.current);
    /* update env-geo to avoid influences */
    env->geo_in_bytes.now = env->dxb_mmap.current;
    env->geo_in_bytes.upper = env->dxb_mmap.limit;
    env_options_adjust_defaults(env);
#ifdef ENABLE_MEMCHECK
    if (prev_limit != env->dxb_mmap.limit || prev_map != env->dxb_mmap.base) {
      VALGRIND_DISCARD(env->valgrind_handle);
      env->valgrind_handle = 0;
      if (env->dxb_mmap.limit)
        env->valgrind_handle = VALGRIND_CREATE_BLOCK(env->dxb_mmap.base, env->dxb_mmap.limit, "mdbx");
    }
#endif /* ENABLE_MEMCHECK */
  } else {
    if (rc != MDBX_UNABLE_EXTEND_MAPSIZE && rc != MDBX_EPERM) {
      ERROR("failed resize datafile/mapping: "
            "present %" PRIuPTR " -> %" PRIuPTR ", "
            "limit %" PRIuPTR " -> %" PRIuPTR ", errcode %d",
            prev_size, size_bytes, prev_limit, limit_bytes, rc);
    } else {
      WARNING("unable resize datafile/mapping: "
              "present %" PRIuPTR " -> %" PRIuPTR ", "
              "limit %" PRIuPTR " -> %" PRIuPTR ", errcode %d",
              prev_size, size_bytes, prev_limit, limit_bytes, rc);
      eASSERT0(env, env->dxb_mmap.limit >= env->dxb_mmap.current);
    }
    if (!env->dxb_mmap.base) {
      env->flags |= ENV_FATAL_ERROR;
      if (env->txn)
        env->txn->flags |= MDBX_TXN_ERROR;
      rc = MDBX_PANIC;
    }
  }

#if IS_WINDOWS
  if (have_exclusive) {
    imports.srwl_ReleaseExclusive(&env->remap_lock);
  } else {
    imports.srwl_ReleaseShared(&env->remap_lock);
  }
  int err = MDBX_SUCCESS;
  if (suspended) {
    err = osal_resume_threads_after_remap(suspended);
    if (suspended != &array_onstack)
      osal_free(suspended);
  }
  /* release the resizer leaf lock LAST (it was acquired first, reverse order) */
  osal_fastmutex_release(&env->resize_mutex);
#else
  if (should_unlock_rdt)
    lck_rdt_unlock(env);
  int err = osal_fastmutex_release(&env->remap_lock);
#endif /* Windows */
  if (err != MDBX_SUCCESS) {
    FATAL("failed resume-after-remap: errcode %d", err);
    return MDBX_PANIC;
  }
  return rc;
}

/* Turn on/off readahead. It's harmful when the DB is larger than RAM. */
__cold int dxb_set_readahead(const MDBX_env *env, const pgno_t edge, const bool enable, const bool force_whole) {
  eASSERT0(env, edge >= NUM_METAS && edge <= MAX_PAGENO + 1);
  eASSERT0(env, (enable & 1) == (enable != 0));
  const bool toggle = force_whole || ((enable ^ env->lck->readahead_anchor) & 1) || !env->lck->readahead_anchor;
  const pgno_t prev_edge = env->lck->readahead_anchor >> 1;
  const size_t limit = env->dxb_mmap.limit;
  size_t offset = toggle ? 0 : pgno_ceil2os_bytes(env, (prev_edge < edge) ? prev_edge : edge);
  offset = (offset < limit) ? offset : limit;

  size_t length = pgno_ceil2os_bytes(env, (prev_edge < edge) ? edge : prev_edge);
  length = (length < limit) ? length : limit;
  length -= offset;

  eASSERT0(env, 0 <= (intptr_t)length);
  if (length == 0)
    return MDBX_SUCCESS;

  NOTICE("readahead %s %u..%u", enable ? "ON" : "OFF", bytes2pgno(env, offset), bytes2pgno(env, offset + length));

#if defined(F_RDAHEAD)
  if (toggle && unlikely(fcntl(env->lazy_fd, F_RDAHEAD, enable) == -1))
    return errno;
#endif /* F_RDAHEAD */

  int err;
  void *const ptr = ptr_disp(env->dxb_mmap.base, offset);
  if (enable) {
#if defined(MADV_NORMAL)
    err = madvise(ptr, length, MADV_NORMAL) ? ignore_enosys_and_eagain(errno) : MDBX_SUCCESS;
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(POSIX_MADV_NORMAL)
    err = ignore_enosys(posix_madvise(ptr, length, POSIX_MADV_NORMAL));
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(POSIX_FADV_NORMAL) && defined(POSIX_FADV_WILLNEED)
    err = ignore_enosys(posix_fadvise(env->lazy_fd, offset, length, POSIX_FADV_NORMAL));
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif IS_WINDOWS
    /* no madvise on Windows */
#else
#warning "FIXME"
#endif
    if (toggle) {
      /* NOTE: Seems there is a bug in the Mach/Darwin/OSX kernel,
       * because MADV_WILLNEED with offset != 0 may cause SIGBUS
       * on following access to the hinted region.
       * 19.6.0 Darwin Kernel Version 19.6.0: Tue Jan 12 22:13:05 PST 2021;
       * root:xnu-6153.141.16~1/RELEASE_X86_64 x86_64 */
#if defined(F_RDADVISE)
      struct radvisory hint;
      hint.ra_offset = offset;
      hint.ra_count = unlikely(length > INT_MAX && sizeof(length) > sizeof(hint.ra_count)) ? INT_MAX : (int)length;
      (void)/* Ignore ENOTTY for DB on the ram-disk and so on */ fcntl(env->lazy_fd, F_RDADVISE, &hint);
#elif defined(MADV_WILLNEED)
      err = madvise(ptr, length, MADV_WILLNEED) ? ignore_enosys_and_eagain(errno) : MDBX_SUCCESS;
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
#elif defined(POSIX_MADV_WILLNEED)
      err = ignore_enosys(posix_madvise(ptr, length, POSIX_MADV_WILLNEED));
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
#elif IS_WINDOWS
      if (imports.PrefetchVirtualMemory) {
        WIN32_MEMORY_RANGE_ENTRY hint;
        hint.VirtualAddress = ptr;
        hint.NumberOfBytes = length;
        (void)imports.PrefetchVirtualMemory(GetCurrentProcess(), 1, &hint, 0);
      }
#elif defined(POSIX_FADV_WILLNEED)
      err = ignore_enosys(posix_fadvise(env->lazy_fd, offset, length, POSIX_FADV_WILLNEED));
      if (unlikely(MDBX_IS_ERROR(err)))
        return err;
#else
#warning "FIXME"
#endif
    }
  } else {
    env_clear_incore_cache(env);
#if defined(MADV_RANDOM)
    err = madvise(ptr, length, MADV_RANDOM) ? ignore_enosys_and_eagain(errno) : MDBX_SUCCESS;
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(POSIX_MADV_RANDOM)
    err = ignore_enosys(posix_madvise(ptr, length, POSIX_MADV_RANDOM));
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif defined(POSIX_FADV_RANDOM)
    err = ignore_enosys(posix_fadvise(env->lazy_fd, offset, length, POSIX_FADV_RANDOM));
    if (unlikely(MDBX_IS_ERROR(err)))
      return err;
#elif IS_WINDOWS
    /* no madvise on Windows */
#else
#warning "FIXME"
#endif /* MADV_RANDOM */
  }

  env->lck->readahead_anchor = (enable & 1) + (edge << 1);
  err = MDBX_SUCCESS;
  return err;
}
