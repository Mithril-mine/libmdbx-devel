/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026
/// \file dxb.c — datafile I/O, header reading, env setup and sync;
///               the resize/readahead path lives in dxb-resize.c.

#include "internals.h"

int dxb_msync(const MDBX_env *env, size_t length_pages, enum osal_syncmode_bits mode_bits) {
  if (MDBX_ENABLE_PGOP_STAT)
    env->lck->pgops.msync.weak += (MDBX_MMAP_NEEDS_JOLT || mode_bits > MDBX_SYNC_NONE);
  return osal_msync(&env->dxb_mmap, pgno_ceil2sp_bytes(env, length_pages), mode_bits);
}

int dxb_fsync(const MDBX_env *env, enum osal_syncmode_bits mode_bits) {
  if (MDBX_ENABLE_PGOP_STAT)
    env->lck->pgops.fsync.weak += (mode_bits > MDBX_SYNC_NONE);
  return osal_fsync(env->lazy_fd, mode_bits);
}

__cold int dxb_pwrite(MDBX_env *env, const void *buf, uint64_t offset) {
#if IS_WINDOWS
  if (env->ioring.overlapped_fd)
    return osal_pwrite_ev(env->ioring.overlapped_fd, env->ioring.async_done, buf, env->ps, offset);
#endif
  return osal_pwrite(env->dxb_mmap.fd, buf, env->ps, offset);
}

__cold int dxb_pread(MDBX_env *env, void *buf, uint64_t offset) {
#if IS_WINDOWS
  if (env->ioring.overlapped_fd)
    return osal_pread_ev(env->ioring.overlapped_fd, env->ioring.async_done, buf, env->ps, offset);
#endif
  return osal_pread(env->dxb_mmap.fd, buf, env->ps, offset);
}

__cold int dxb_read_header(MDBX_env *env, meta_t *dest, const int lck_exclusive, const mdbx_mode_t mode_bits,
                           const int preopen_mode) {
  memset(dest, 0, sizeof(meta_t));
  int rc = osal_filesize(env->lazy_fd, &env->dxb_mmap.filesize);
  if (unlikely(rc != MDBX_SUCCESS))
    return rc;

  unaligned_poke_u64(4, dest->sign, DATASIGN_WEAK);
  rc = MDBX_CORRUPTED;

  /* Read twice all meta pages so we can find the latest one. */
  unsigned loop_limit = NUM_METAS * 2;
  /* We don't know the page size on first time. So, just guess it. */
  unsigned guess_pagesize = 0;
  for (unsigned loop_count = 0; loop_count < loop_limit; ++loop_count) {
    const unsigned meta_number = loop_count % NUM_METAS;
    const unsigned offset = (guess_pagesize             ? guess_pagesize
                             : (loop_count > NUM_METAS) ? env->ps
                                                        : globals.sys_pagesize) *
                            meta_number;

    char buffer[MDBX_MIN_PAGESIZE];
    unsigned retryleft = 42;
    while (1) {
      TRACE("reading meta[%d]: offset %u, bytes %u, retry-left %u", meta_number, offset, MDBX_MIN_PAGESIZE, retryleft);
      int err = osal_pread(env->lazy_fd, buffer, MDBX_MIN_PAGESIZE, offset);
      if (err == MDBX_ENODATA && offset == 0 && loop_count == 0 && env->dxb_mmap.filesize == 0 &&
          mode_bits /* non-zero for DB creation */ != 0) {
        NOTICE("read meta: empty file (%d, %s)", err, mdbx_strerror(err));
        return err;
      }
#if IS_WINDOWS
      if (err == ERROR_LOCK_VIOLATION) {
        SleepEx(0, true);
        err = osal_pread(env->lazy_fd, buffer, MDBX_MIN_PAGESIZE, offset);
        if (err == ERROR_LOCK_VIOLATION && --retryleft) {
          WARNING("read meta[%u,%u]: %i, %s", offset, MDBX_MIN_PAGESIZE, err, mdbx_strerror(err));
          continue;
        }
        /* A persistent lock violation while opening exclusively means the data
         * file is held by another exclusive env — possibly one reached through
         * a different hard-link, hence with its own lock file, so lck_setup()
         * could not detect the contention. Report the documented MDBX_BUSY
         * rather than the raw winerror. (issue #179) */
        if (err == ERROR_LOCK_VIOLATION && env_exclusive_fastpath(env))
          return MDBX_BUSY;
      }
#endif /* Windows */
      if (err != MDBX_SUCCESS) {
        ERROR("read meta[%u,%u]: %i, %s", offset, MDBX_MIN_PAGESIZE, err, mdbx_strerror(err));
        return err;
      }

      char again[MDBX_MIN_PAGESIZE];
      err = osal_pread(env->lazy_fd, again, MDBX_MIN_PAGESIZE, offset);
#if IS_WINDOWS
      if (err == ERROR_LOCK_VIOLATION) {
        SleepEx(0, true);
        err = osal_pread(env->lazy_fd, again, MDBX_MIN_PAGESIZE, offset);
        if (err == ERROR_LOCK_VIOLATION && --retryleft) {
          WARNING("read meta[%u,%u]: %i, %s", offset, MDBX_MIN_PAGESIZE, err, mdbx_strerror(err));
          continue;
        }
        if (err == ERROR_LOCK_VIOLATION && env_exclusive_fastpath(env))
          return MDBX_BUSY; /* see the note above */
      }
#endif /* Windows */
      if (err != MDBX_SUCCESS) {
        ERROR("read meta[%u,%u]: %i, %s", offset, MDBX_MIN_PAGESIZE, err, mdbx_strerror(err));
        return err;
      }

      if (memcmp(buffer, again, MDBX_MIN_PAGESIZE) == 0 || --retryleft == 0)
        break;

      VERBOSE("meta[%u] was updated, re-read it", meta_number);
    }

    if (!retryleft) {
      ERROR("meta[%u] is too volatile, skip it", meta_number);
      continue;
    }

    page_t *const page = (page_t *)buffer;
    meta_t *const meta = page_meta(page);
    rc = meta_validate(env, meta, page, meta_number, &guess_pagesize);
    if (rc != MDBX_SUCCESS)
      continue;

    bool latch;
    if (env->stuck_meta >= 0)
      latch = (meta_number == (unsigned)env->stuck_meta);
    else if (preopen_mode == MDBX_PREOPEN_NONE)
      /* Regular open: prefer a weak checkpoint of the current boot. Whether
       * it is usable is decided below by the lock mode (meta_weak_acceptable):
       * a boot-id match means the data is live, a writable open can
       * steady-sync/promote it, a read-only one requires recovery. The
       * presence of a lock file must not steer this choice: lckless and
       * with-lck opens behave identically here. (issue #215) */
      latch = meta_bootid_match(meta) ? meta_choice_recent(meta->unsafe_txnid, SIGN_IS_STEADY(meta->unsafe_sign),
                                                           dest->unsafe_txnid, SIGN_IS_STEADY(dest->unsafe_sign))
                                      : meta_choice_steady(meta->unsafe_txnid, SIGN_IS_STEADY(meta->unsafe_sign),
                                                           dest->unsafe_txnid, SIGN_IS_STEADY(dest->unsafe_sign));
    else
      switch (preopen_mode) {
      case MDBX_PREOPEN_LEGACY:
      case MDBX_PREOPEN_PROPERLY:
        /* The newest meta-page regardless of its nature/sign, i.e. a raw
         * "most recent" choice: a weak checkpoint on top leads to
         * MDBX_CORRUPTED (legacy) or MDBX_WANNA_RECOVERY (properly) below. */
        latch = meta_choice_recent(meta->unsafe_txnid, SIGN_IS_STEADY(meta->unsafe_sign), dest->unsafe_txnid,
                                   SIGN_IS_STEADY(dest->unsafe_sign));
        break;
      case MDBX_PREOPEN_PESSIMISTIC:
        /* The newest steady checkpoint regardless of the boot-id. */
        latch = meta_choice_steady(meta->unsafe_txnid, SIGN_IS_STEADY(meta->unsafe_sign), dest->unsafe_txnid,
                                   SIGN_IS_STEADY(dest->unsafe_sign));
        break;
      case MDBX_PREOPEN_OPTIMISTIC: {
        /* A weak checkpoint with a matching boot-id is treated as if it was
         * steady, so the newest usable meta-page wins (usable dominates, then
         * the larger txnid). */
        const bool a_usable = SIGN_IS_STEADY(meta->unsafe_sign) || meta_bootid_match(meta);
        const bool b_usable = SIGN_IS_STEADY(dest->unsafe_sign) || meta_bootid_match(dest);
        latch = meta_choice_steady(meta->unsafe_txnid, a_usable, dest->unsafe_txnid, b_usable);
        break;
      }
      default:
        latch = false;
      }
    if (latch) {
      *dest = *meta;
      if (!lck_exclusive && !meta_is_steady(dest))
        loop_limit += 1; /* LY: should re-read to hush race with update */
      VERBOSE("latch meta[%u]", meta_number);
    }
  }

  if (dest->pagesize == 0) {
    ERROR("%s", "no meta-pages, database is corrupted");
    if (!MDBX_IS_ERROR(rc)) {
      /* TODO: try to restore the database by fully checking b-tree structure
       * for the each meta page, if the corresponding option was given */
      return MDBX_CORRUPTED;
    }
    return rc;
  }

  if (preopen_mode == MDBX_PREOPEN_PROPERLY && !meta_is_steady(dest)) {
    ERROR("%s", "the newest checkpoint is not steady, recovery is required");
    return MDBX_WANNA_RECOVERY;
  }

  if (env->stuck_meta < 0 && !(meta_is_steady(dest) || meta_weak_acceptable(env, dest, lck_exclusive) ||
                               (preopen_mode == MDBX_PREOPEN_OPTIMISTIC && meta_bootid_match(dest)))) {
    ERROR("%s", "no usable meta-pages, database is corrupted");
    if (!MDBX_IS_ERROR(rc)) {
      /* TODO: try to restore the database by fully checking b-tree structure
       * for the each meta page, if the corresponding option was given */
      return MDBX_CORRUPTED;
    }
    return rc;
  }

  return MDBX_SUCCESS;
}

#if defined(ENABLE_MEMCHECK) || defined(__SANITIZE_ADDRESS__)
void dxb_sanitize_tail(MDBX_env *env, MDBX_txn *txn) {
  if (!RUNNING_ON_ASAN && !mdbx_running_on_Valgrind())
    return;
  if (txn) { /* transaction start */
    if (env->poison_edge < txn->geo.first_unallocated)
      env->poison_edge = txn->geo.first_unallocated;
    VALGRIND_MAKE_MEM_DEFINED(env->dxb_mmap.base, pgno2bytes(env, txn->geo.first_unallocated));
    MDBX_ASAN_UNPOISON_MEMORY_REGION(env->dxb_mmap.base, pgno2bytes(env, txn->geo.first_unallocated));
    /* don't touch more, it should be already poisoned */
  } else { /* transaction end */
    bool should_unlock = false;
    pgno_t last = MAX_PAGENO + 1;
    if (env->pid != osal_getpid()) {
      /* resurrect after fork */
      return;
    } else if ((env->flags & MDBX_RDONLY) != 0 || !env->basal_txn) {
      /* read-only mode, no write-txn, no wlock mutex */
      last = NUM_METAS;
    } else if ((env->flags & MDBX_NOSTICKYTHREADS) ? (env->basal_txn->owner != 0)
                                                   : (env->basal_txn->owner == osal_thread_self())) {
      /* inside write-txn */
      last = meta_recent(env, &env->basal_txn->wr.troika).ptr_v->geometry.first_unallocated;
    } else if (lck_txn_lock(env, true) == MDBX_SUCCESS) {
      /* no write-txn */
      last = NUM_METAS;
      should_unlock = true;
    } else {
      /* write txn is running, therefore shouldn't poison any memory range */
      return;
    }

    last = mvcc_largest_this(env, last);
    const pgno_t edge = env->poison_edge;
    if (edge > last) {
      eASSERT0(env, last >= NUM_METAS);
      env->poison_edge = last;
      VALGRIND_MAKE_MEM_NOACCESS(ptr_disp(env->dxb_mmap.base, pgno2bytes(env, last)), pgno2bytes(env, edge - last));
      MDBX_ASAN_POISON_MEMORY_REGION(ptr_disp(env->dxb_mmap.base, pgno2bytes(env, last)), pgno2bytes(env, edge - last));
    }
    if (should_unlock)
      lck_txn_unlock(env);
  }
}
#endif /* ENABLE_MEMCHECK || __SANITIZE_ADDRESS__ */

__cold int dxb_setup(MDBX_env *env, const int lck_rc, const mdbx_mode_t mode_bits) {
  meta_t header;
  eASSERT0(env, !(env->flags & ENV_ACTIVE));
  int rc = MDBX_RESULT_FALSE;
  int err = dxb_read_header(env, &header, lck_rc, mode_bits, MDBX_PREOPEN_NONE);
  if (unlikely(err != MDBX_SUCCESS)) {
    if (lck_rc != /* lck exclusive */ MDBX_RESULT_TRUE || err != MDBX_ENODATA || (env->flags & MDBX_RDONLY) != 0 ||
        /* recovery mode */ env->stuck_meta >= 0)
      return LOG_IFERR(err);

    DEBUG("%s", "create new database");
    rc = /* new database */ MDBX_RESULT_TRUE;

    if (!env->geo_in_bytes.now) {
      /* set defaults if not configured */
      err = mdbx_env_set_geometry(env, 0, -1, -1, -1, -1, -1);
      if (unlikely(err != MDBX_SUCCESS))
        return LOG_IFERR(err);
    }

    err = env_page_auxbuffer(env);
    if (unlikely(err != MDBX_SUCCESS))
      return LOG_IFERR(err);

    header = *meta_init_triplet(env, env->page_auxbuf);
    err = osal_pwrite(env->lazy_fd, env->page_auxbuf, env->ps * (size_t)NUM_METAS, 0);
    if (unlikely(err != MDBX_SUCCESS))
      return LOG_IFERR(err);

    err = osal_fsetsize(env->lazy_fd, env->dxb_mmap.filesize = env->dxb_mmap.current = env->geo_in_bytes.now);
    if (unlikely(err != MDBX_SUCCESS))
      return LOG_IFERR(err);

#if MDBX_CHECKING > 0 || MDBX_DEBUG > 0
    err = dxb_read_header(env, &header, lck_rc, mode_bits, MDBX_PREOPEN_NONE);
    if (unlikely(err != MDBX_SUCCESS))
      return LOG_IFERR(err);
#endif /* MDBX_CHECKING > 0 || MDBX_DEBUG > 0 */
  }

  VERBOSE("header: root %" PRIaPGNO "/%" PRIaPGNO ", geo %" PRIaPGNO "/%" PRIaPGNO "-%" PRIaPGNO "/%" PRIaPGNO
          " +%u -%u, txn_id %" PRIaTXN ", %s",
          header.trees.main.root, header.trees.gc.root, header.geometry.lower, header.geometry.first_unallocated,
          header.geometry.now, header.geometry.upper, pv2pages(header.geometry.grow_pv),
          pv2pages(header.geometry.shrink_pv), unaligned_peek_u64(4, header.txnid_a), durable_caption(&header));

  if (unlikely((header.trees.gc.flags & DB_PERSISTENT_FLAGS) != MDBX_INTEGERKEY)) {
    ERROR("unexpected/invalid db-flags 0x%x for %s", header.trees.gc.flags, "GC/FreeDB");
    return MDBX_INCOMPATIBLE;
  }
  env->dbs_flags[FREE_DBI] = DB_VALID | MDBX_INTEGERKEY;
  env->kvs[FREE_DBI].clc.k.cmp = ncmp_uint_align4; /* aligned MDBX_INTEGERKEY */
  env->kvs[FREE_DBI].clc.k.lmax = env->kvs[FREE_DBI].clc.k.lmin = 8;
  env->kvs[FREE_DBI].clc.v.cmp = ncmp_lenfast;
  env->kvs[FREE_DBI].clc.v.lmin = 4;
  env->kvs[FREE_DBI].clc.v.lmax = mdbx_env_get_maxvalsize_ex(env, MDBX_INTEGERKEY);
  clc_reset_methods(&env->kvs[FREE_DBI].clc.k);
  clc_reset_methods(&env->kvs[FREE_DBI].clc.v);

  if (env->ps != header.pagesize)
    env_setup_pagesize(env, header.pagesize);
  if ((env->flags & MDBX_RDONLY) == 0) {
    err = env_page_auxbuffer(env);
    if (unlikely(err != MDBX_SUCCESS))
      return LOG_IFERR(err);
  }

  /* With the exclusive fast i/o path the data file is shareable, so exclusivity
   * must be established by the constant meta/body byte-range locks before this
   * function mutates an *existing* database: the geometry application below and
   * the resize/mmap (osal_fsetsize + osal_mmap SETLENGTH) at the end. env->ps is
   * known only after the non-mutating header read (and env_setup_pagesize just
   * above), which is why the locks are taken here rather than earlier.
   *
   * The new-database branch above does write the meta triplet and set the file
   * size before this point, but that path is only reached for an empty file
   * (dxb_read_header returned ENODATA), which cannot have a concurrent
   * without-lck reader holding a shared data-file lock, and a competing creator
   * is already rejected at the lock-file level (lck_seize + the MDBX_EXCLUSIVE
   * check in lck_setup). So no mutation of a database visible to another env
   * can happen before the locks are taken.
   *
   * A conflicting lock (another exclusive env — including one reached through a
   * different hard-link with its own lock file — or a without-lck reader) makes
   * this fail with MDBX_BUSY before the existing database is resized/remapped.
   * No-op unless env_exclusive_fastpath(). (issue #179) */
  err = windows_seize_exclusive(env);
  if (unlikely(MDBX_IS_ERROR(err)))
    return LOG_IFERR(err);

  size_t expected_filesize = 0;
  const size_t allocated_bytes = pgno2bytes(env, header.geometry.first_unallocated);
  const size_t allocated_aligned2os_bytes = ceil_powerof2(allocated_bytes, globals.sys_allocation_granularity);
  if ((env->flags & MDBX_RDONLY)    /* readonly */
      || lck_rc != MDBX_RESULT_TRUE /* not exclusive */
      || /* recovery mode */ env->stuck_meta >= 0) {
    /* use present params from db */
    const size_t pagesize = header.pagesize;
    err = mdbx_env_set_geometry(env, header.geometry.lower * pagesize, header.geometry.now * pagesize,
                                header.geometry.upper * pagesize, pv2pages(header.geometry.grow_pv) * pagesize,
                                pv2pages(header.geometry.shrink_pv) * pagesize, header.pagesize);
    if (unlikely(err != MDBX_SUCCESS)) {
      ERROR("%s: err %d", "could not apply geometry from db", err);
      return (err == MDBX_EINVAL) ? MDBX_INCOMPATIBLE : err;
    }
  } else if (env->geo_in_bytes.now) {
    /* silently growth to last used page */
    if (env->geo_in_bytes.now < allocated_aligned2os_bytes)
      env->geo_in_bytes.now = allocated_aligned2os_bytes;
    if (env->geo_in_bytes.upper < allocated_aligned2os_bytes)
      env->geo_in_bytes.upper = allocated_aligned2os_bytes;

    /* apply preconfigured params, but only if substantial changes:
     *  - upper or lower limit changes
     *  - shrink threshold or growth step
     * But ignore change just a 'now/current' size. */
    if (bytes_ceil2os_bytes(env, env->geo_in_bytes.upper) != pgno2bytes(env, header.geometry.upper) ||
        bytes_ceil2os_bytes(env, env->geo_in_bytes.lower) != pgno2bytes(env, header.geometry.lower) ||
        bytes_ceil2os_bytes(env, env->geo_in_bytes.shrink) != pgno2bytes(env, pv2pages(header.geometry.shrink_pv)) ||
        bytes_ceil2os_bytes(env, env->geo_in_bytes.grow) != pgno2bytes(env, pv2pages(header.geometry.grow_pv))) {

      if (env->geo_in_bytes.shrink && env->geo_in_bytes.now > allocated_bytes)
        /* pre-shrink if enabled */
        env->geo_in_bytes.now = allocated_bytes + env->geo_in_bytes.shrink - allocated_bytes % env->geo_in_bytes.shrink;

      /* сейчас БД еще не открыта, поэтому этот вызов не изменит геометрию, но проверит и скорректирует параметры
       * с учетом реального размера страницы. */
      err = mdbx_env_set_geometry(env, env->geo_in_bytes.lower, env->geo_in_bytes.now, env->geo_in_bytes.upper,
                                  env->geo_in_bytes.grow, env->geo_in_bytes.shrink, header.pagesize);
      if (unlikely(err != MDBX_SUCCESS)) {
        ERROR("%s: err %d", "could not apply preconfigured db-geometry", err);
        return (err == MDBX_EINVAL) ? MDBX_INCOMPATIBLE : err;
      }

      /* altering fields to match geometry given from user */
      expected_filesize = pgno_ceil2os_bytes(env, header.geometry.now);
      header.geometry.now = (pgno_t)bytes_ceil2os_pgno(env, env->geo_in_bytes.now);
      header.geometry.lower = (pgno_t)bytes_ceil2os_pgno(env, env->geo_in_bytes.lower);
      header.geometry.upper = (pgno_t)bytes_ceil2os_pgno(env, env->geo_in_bytes.upper);
      header.geometry.grow_pv = pages2pv(bytes_ceil2os_pgno(env, env->geo_in_bytes.grow));
      header.geometry.shrink_pv = pages2pv(bytes_ceil2os_pgno(env, env->geo_in_bytes.shrink));

      VERBOSE("amending: root %" PRIaPGNO "/%" PRIaPGNO ", geo %" PRIaPGNO "/%" PRIaPGNO "-%" PRIaPGNO "/%" PRIaPGNO
              " +%u -%u, txn_id %" PRIaTXN ", %s",
              header.trees.main.root, header.trees.gc.root, header.geometry.lower, header.geometry.first_unallocated,
              header.geometry.now, header.geometry.upper, pv2pages(header.geometry.grow_pv),
              pv2pages(header.geometry.shrink_pv), unaligned_peek_u64(4, header.txnid_a), durable_caption(&header));
    } else {
      /* fetch back 'now/current' size, since it was ignored during comparison and may differ. */
      env->geo_in_bytes.now = pgno_ceil2os_bytes(env, header.geometry.now);
    }
    ENSURE_OBJ(env, header.geometry.now >= header.geometry.first_unallocated);
  } else {
    /* geo-params are not pre-configured by user, get current values from the meta. */
    env->geo_in_bytes.now = pgno_ceil2os_bytes(env, header.geometry.now);
    env->geo_in_bytes.lower = pgno_ceil2os_bytes(env, header.geometry.lower);
    env->geo_in_bytes.upper = pgno_ceil2os_bytes(env, header.geometry.upper);
    env->geo_in_bytes.grow = pgno_ceil2os_bytes(env, pv2pages(header.geometry.grow_pv));
    env->geo_in_bytes.shrink = pgno_ceil2os_bytes(env, pv2pages(header.geometry.shrink_pv));
  }

  ENSURE_OBJ(env, env->geo_in_bytes.now >= allocated_bytes);
  if (!expected_filesize)
    expected_filesize = env->geo_in_bytes.now;
  const uint64_t filesize_before = env->dxb_mmap.filesize;
  if (unlikely(filesize_before != env->geo_in_bytes.now)) {
    /* Two directions are possible and must not be conflated (issue #154):
     *  - `filesize > geo.now`: another process has grown the file, OR our own
     *    earlier commit deferred a shrink (on Windows the mapping cannot be
     *    un-mapped while readers exist, so the meta was written with the
     *    reduced `now` while the file kept its size);
     *  - `filesize < geo.now`: the file is genuinely smaller than the meta
     *    claims and must be grown to match. */
    const char *const direction = (filesize_before > env->geo_in_bytes.now) ? "larger" : "smaller";
    if (lck_rc != /* lck exclusive */ MDBX_RESULT_TRUE) {
      VERBOSE("filesize mismatch (expect %" PRIuPTR "b/%" PRIaPGNO "p, have %" PRIu64 "b/%" PRIu64
              "p, %s), assume other process working or a deferred shrink",
              env->geo_in_bytes.now, bytes2pgno(env, env->geo_in_bytes.now), filesize_before,
              filesize_before >> env->ps2ln, direction);
    } else {
      if (filesize_before != expected_filesize)
        WARNING("filesize mismatch (expect %" PRIuSIZE "b/%" PRIaPGNO "p, have %" PRIu64 "b/%" PRIu64 "p, %s)",
                expected_filesize, bytes2pgno(env, expected_filesize), filesize_before, filesize_before >> env->ps2ln,
                direction);
      if (filesize_before < allocated_bytes) {
        ERROR("last-page beyond end-of-file (last %" PRIaPGNO ", have %" PRIaPGNO ")",
              header.geometry.first_unallocated, bytes2pgno(env, (size_t)filesize_before));
        return MDBX_CORRUPTED;
      }

      if (env->flags & MDBX_RDONLY) {
        if (filesize_before & (globals.sys_pagesize - 1)) {
          ERROR("filesize should be rounded-up to system page size %u", globals.sys_pagesize);
          return MDBX_WANNA_RECOVERY;
        }
        WARNING("%s", "ignore filesize mismatch in readonly-mode");
      } else {
        VERBOSE("will resize datafile to %" PRIuSIZE " bytes, %" PRIaPGNO " pages", env->geo_in_bytes.now,
                bytes2pgno(env, env->geo_in_bytes.now));
      }
    }
  }

  VERBOSE("current boot-id %" PRIx64 "-%" PRIx64 " (%savailable)", globals.bootid.x, globals.bootid.y,
          (globals.bootid.x | globals.bootid.y) ? "" : "not-");

  /* calculate readahead hint before mmap with zero redundant pages */
  const bool readahead =
      !(env->flags & MDBX_NORDAHEAD) && mdbx_is_readahead_reasonable(allocated_bytes, 0) == MDBX_RESULT_TRUE;

  err = osal_mmap(env->flags, &env->dxb_mmap, env->geo_in_bytes.now, env->geo_in_bytes.upper,
                  (lck_rc && env->stuck_meta < 0) ? MMAP_OPTION_SETLENGTH : 0, env->pathname.dxb);
  if (unlikely(err != MDBX_SUCCESS))
    return LOG_IFERR(err);

#if defined(MADV_DONTDUMP)
  err =
      madvise(env->dxb_mmap.base, env->dxb_mmap.limit, MADV_DONTDUMP) ? ignore_enosys_and_eagain(errno) : MDBX_SUCCESS;
  if (unlikely(MDBX_IS_ERROR(err)))
    return LOG_IFERR(err);
#endif /* MADV_DONTDUMP */
#if defined(MADV_DODUMP)
  if (globals.runtime_flags & MDBX_DBG_DUMP) {
    const size_t meta_length_aligned2os = pgno_ceil2os_bytes(env, NUM_METAS);
    err = madvise(env->dxb_mmap.base, meta_length_aligned2os, MADV_DODUMP) ? ignore_enosys_and_eagain(errno)
                                                                           : MDBX_SUCCESS;
    if (unlikely(MDBX_IS_ERROR(err)))
      return LOG_IFERR(err);
  }
#endif /* MADV_DODUMP */

#ifdef ENABLE_MEMCHECK
  env->valgrind_handle = VALGRIND_CREATE_BLOCK(env->dxb_mmap.base, env->dxb_mmap.limit, "mdbx");
#endif /* ENABLE_MEMCHECK */

  eASSERT0(env, allocated_bytes >= pgno2bytes(env, NUM_METAS) && allocated_bytes <= env->dxb_mmap.limit);
#if defined(ENABLE_MEMCHECK) || defined(__SANITIZE_ADDRESS__)
  if (env->dxb_mmap.filesize > allocated_bytes && env->dxb_mmap.filesize < env->dxb_mmap.limit) {
    VALGRIND_MAKE_MEM_NOACCESS(ptr_disp(env->dxb_mmap.base, allocated_bytes), env->dxb_mmap.filesize - allocated_bytes);
    MDBX_ASAN_POISON_MEMORY_REGION(ptr_disp(env->dxb_mmap.base, allocated_bytes),
                                   env->dxb_mmap.filesize - allocated_bytes);
  }
  env->poison_edge =
      bytes2pgno(env, (env->dxb_mmap.filesize < env->dxb_mmap.limit) ? env->dxb_mmap.filesize : env->dxb_mmap.limit);
#endif /* ENABLE_MEMCHECK || __SANITIZE_ADDRESS__ */

  troika_t troika = meta_tap(env);
#if MDBX_DEBUG > 0
  meta_troika_dump(env, &troika);
#endif
  //-------------------------------- validate/rollback head & steady meta-pages
  if (unlikely(env->stuck_meta >= 0)) {
    /* recovery mode */
    meta_t clone;
    meta_t const *const target = METAPAGE(env, env->stuck_meta);
    err = meta_validate_copy(env, target, &clone);
    if (unlikely(err != MDBX_SUCCESS)) {
      ERROR("target meta[%u] is corrupted", bytes2pgno(env, ptr_dist(payload2page(target), env->dxb_mmap.base)));
      meta_troika_dump(env, &troika);
      return MDBX_CORRUPTED;
    }
  } else /* not recovery mode */
    while (1) {
      const unsigned meta_clash_mask = meta_eq_mask(&troika);
      if (unlikely(meta_clash_mask)) {
        ERROR("meta-pages are clashed: mask 0x%d", meta_clash_mask);
        meta_troika_dump(env, &troika);
        return MDBX_CORRUPTED;
      }

      if (lck_rc != /* lck exclusive */ MDBX_RESULT_TRUE) {
        /* non-exclusive mode,
         * meta-pages should be validated by a first process opened the DB */
        if (troika.recent == troika.prefer_steady)
          break;

        if (!env->lck_mmap.lck) {
          /* LY: without-lck (read-only) mode, so it is impossible that other
           * process made weak checkpoint. */
          ERROR("%s", "without-lck, unable recovery/rollback");
          meta_troika_dump(env, &troika);
          return MDBX_WANNA_RECOVERY;
        }

        /* LY: assume just have a collision with other running process,
         *     or someone make a weak checkpoint */
        VERBOSE("%s", "assume collision or online weak checkpoint");
        break;
      }
      eASSERT0(env, lck_rc == MDBX_RESULT_TRUE);
      /* exclusive mode */

      const meta_ptr_t recent = meta_recent(env, &troika);
      const meta_ptr_t prefer_steady = meta_prefer_steady(env, &troika);
      meta_t clone;
      if (prefer_steady.is_steady) {
        err = meta_validate_copy(env, prefer_steady.ptr_c, &clone);
        if (unlikely(err != MDBX_SUCCESS)) {
          ERROR("meta[%u] with %s txnid %" PRIaTXN " is corrupted, %s needed",
                bytes2pgno(env, ptr_dist(prefer_steady.ptr_c, env->dxb_mmap.base)), "steady", prefer_steady.txnid,
                "manual recovery");
          meta_troika_dump(env, &troika);
          return MDBX_CORRUPTED;
        }
        if (prefer_steady.ptr_c == recent.ptr_c)
          break;
      }

      const pgno_t pgno = bytes2pgno(env, ptr_dist(recent.ptr_c, env->dxb_mmap.base));
      const bool last_valid = meta_validate_copy(env, recent.ptr_c, &clone) == MDBX_SUCCESS;
      eASSERT0(env, !prefer_steady.is_steady || recent.txnid != prefer_steady.txnid);
      if (unlikely(!last_valid)) {
        if (unlikely(!prefer_steady.is_steady)) {
          ERROR("%s for open or automatic rollback, %s", "there are no suitable meta-pages",
                "manual recovery is required");
          meta_troika_dump(env, &troika);
          return MDBX_CORRUPTED;
        }
        WARNING("meta[%u] with last txnid %" PRIaTXN " is corrupted, rollback needed", pgno, recent.txnid);
        meta_troika_dump(env, &troika);
        goto purge_meta_head;
      }

      if (meta_bootid_match(recent.ptr_c)) {
        if (env->flags & MDBX_RDONLY) {
          ERROR("%s, but boot-id(%016" PRIx64 "-%016" PRIx64 ") is MATCH: "
                "rollback NOT needed, steady-sync NEEDED%s",
                "opening after an unclean shutdown", globals.bootid.x, globals.bootid.y,
                ", but unable in read-only mode");
          meta_troika_dump(env, &troika);
          return MDBX_WANNA_RECOVERY;
        }
        WARNING("%s, but boot-id(%016" PRIx64 "-%016" PRIx64 ") is MATCH: "
                "rollback NOT needed, steady-sync NEEDED%s",
                "opening after an unclean shutdown", globals.bootid.x, globals.bootid.y, "");
        header = clone;
        env->lck->unsynced_pages.weak = header.geometry.first_unallocated;
        if (!env->lck->eoos_timestamp.weak)
          env->lck->eoos_timestamp.weak = osal_monotime();
        break;
      }
      if (unlikely(!prefer_steady.is_steady)) {
        ERROR("%s, but %s for automatic rollback: %s", "opening after an unclean shutdown",
              "there are no suitable meta-pages", "manual recovery is required");
        meta_troika_dump(env, &troika);
        return MDBX_CORRUPTED;
      }

    purge_meta_head:
      if (env->flags & MDBX_RDONLY) {
        ERROR("%s and rollback needed: (from head %" PRIaTXN " to steady %" PRIaTXN ")%s",
              "opening after an unclean shutdown", recent.txnid, prefer_steady.txnid, ", but unable in read-only mode");
        meta_troika_dump(env, &troika);
        return MDBX_WANNA_RECOVERY;
      }
      NOTICE("%s and doing automatic rollback: "
             "purge%s meta[%u] with%s txnid %" PRIaTXN,
             "opening after an unclean shutdown", last_valid ? "" : " invalid", pgno, last_valid ? " weak" : "",
             recent.txnid);
      meta_troika_dump(env, &troika);
      ENSURE_OBJ(env, prefer_steady.is_steady);
      err = meta_override(env, pgno, 0, last_valid ? recent.ptr_c : prefer_steady.ptr_c);
      if (err) {
        ERROR("rollback: overwrite meta[%u] with txnid %" PRIaTXN ", error %d", pgno, recent.txnid, err);
        return err;
      }
      troika = meta_tap(env);
      ENSURE_OBJ(env, 0 == meta_txnid(recent.ptr_v));
      ENSURE_OBJ(env, 0 == meta_eq_mask(&troika));
    }

  if (lck_rc == /* lck exclusive */ MDBX_RESULT_TRUE) {
    //-------------------------------------------------- shrink DB & update geo
    /* re-check size after mmap */
    if (floor_powerof2(env->dxb_mmap.current, globals.sys_pagesize) < allocated_bytes) {
      ERROR("unacceptable/unexpected datafile size %" PRIuPTR, env->dxb_mmap.current);
      return MDBX_PROBLEM;
    }
    if (env->dxb_mmap.current != env->geo_in_bytes.now) {
      header.geometry.now = bytes2pgno(env, env->geo_in_bytes.now);
      NOTICE("need update meta-geo to filesize %" PRIuPTR " bytes, aligned %" PRIaPGNO " pages", env->geo_in_bytes.now,
             header.geometry.now);
    }

    const meta_ptr_t recent = meta_recent(env, &troika);
    if (/* не учитываем различия в geo.first_unallocated */
        header.geometry.grow_pv != recent.ptr_c->geometry.grow_pv ||
        header.geometry.shrink_pv != recent.ptr_c->geometry.shrink_pv ||
        header.geometry.lower != recent.ptr_c->geometry.lower ||
        header.geometry.upper != recent.ptr_c->geometry.upper || header.geometry.now != recent.ptr_c->geometry.now) {
      if ((env->flags & MDBX_RDONLY) != 0 ||
          /* recovery mode */ env->stuck_meta >= 0) {
        WARNING("skipped update meta.geo in %s mode: from l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO
                "/s%u-g%u, to l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO "/s%u-g%u",
                (env->stuck_meta < 0) ? "read-only" : "recovery", recent.ptr_c->geometry.lower,
                recent.ptr_c->geometry.now, recent.ptr_c->geometry.upper, pv2pages(recent.ptr_c->geometry.shrink_pv),
                pv2pages(recent.ptr_c->geometry.grow_pv), header.geometry.lower, header.geometry.now,
                header.geometry.upper, pv2pages(header.geometry.shrink_pv), pv2pages(header.geometry.grow_pv));
      } else {
        const txnid_t next_txnid = safe64_txnid_next(recent.txnid);
        if (unlikely(next_txnid > MAX_TXNID)) {
          ERROR("txnid overflow, raise %d", MDBX_TXN_FULL);
          return MDBX_TXN_FULL;
        }
        NOTICE("updating meta.geo: "
               "from l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO "/s%u-g%u (txn#%" PRIaTXN "), "
               "to l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO "/s%u-g%u (txn#%" PRIaTXN ")",
               recent.ptr_c->geometry.lower, recent.ptr_c->geometry.now, recent.ptr_c->geometry.upper,
               pv2pages(recent.ptr_c->geometry.shrink_pv), pv2pages(recent.ptr_c->geometry.grow_pv), recent.txnid,
               header.geometry.lower, header.geometry.now, header.geometry.upper, pv2pages(header.geometry.shrink_pv),
               pv2pages(header.geometry.grow_pv), next_txnid);

        if (unlikely(header.unsafe_txnid != recent.txnid)) {
          const pgno_t recent_pgno = bytes2pgno(env, ptr_dist(recent.ptr_c, env->dxb_mmap.base));
          ERROR("meta[%u] recent steady txnid %" PRIaTXN " != header txnid %" PRIaTXN
                ", this is too unexpected and requires manual analysis.",
                recent_pgno, recent.txnid, header.unsafe_txnid);
          return MDBX_PROBLEM;
        }
        meta_set_txnid(env, &header, next_txnid);
        err = dxb_sync_locked(env, env->flags | txn_unmap_allowed, &header, &troika);
        if (err) {
          ERROR("error %d, while updating meta.geo: "
                "from l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO "/s%u-g%u (txn#%" PRIaTXN "), "
                "to l%" PRIaPGNO "-n%" PRIaPGNO "-u%" PRIaPGNO "/s%u-g%u (txn#%" PRIaTXN ")",
                err, recent.ptr_c->geometry.lower, recent.ptr_c->geometry.now, recent.ptr_c->geometry.upper,
                pv2pages(recent.ptr_c->geometry.shrink_pv), pv2pages(recent.ptr_c->geometry.grow_pv), recent.txnid,
                header.geometry.lower, header.geometry.now, header.geometry.upper, pv2pages(header.geometry.shrink_pv),
                pv2pages(header.geometry.grow_pv), header.unsafe_txnid);
          return err;
        }
      }
    }

    atomic_store32(&env->lck->discarded_tail, bytes2pgno(env, allocated_aligned2os_bytes), mo_Relaxed);

    if ((env->flags & MDBX_RDONLY) == 0 && env->stuck_meta < 0 &&
        (globals.runtime_flags & MDBX_DBG_DONT_UPGRADE) == 0) {
      for (unsigned n = 0; n < NUM_METAS; ++n) {
        meta_t *const meta = METAPAGE(env, n);
        if (unlikely(unaligned_peek_u64(4, &meta->magic_and_version) != MDBX_DATA_MAGIC) ||
            (meta->dxbid.x | meta->dxbid.y) == 0 || (meta->gc_flags & ~DB_PERSISTENT_FLAGS)) {
          const txnid_t txnid = meta_is_used(&troika, n) ? constmeta_txnid(meta) : 0;
          NOTICE("%s %s"
                 "meta[%u], txnid %" PRIaTXN,
                 "updating db-format/guid signature for", meta_is_steady(meta) ? "stead-" : "weak-", n, txnid);
          err = meta_override(env, n, txnid, meta);
          if (unlikely(err != MDBX_SUCCESS) &&
              /* Just ignore the MDBX_PROBLEM error, since here it is
               * returned only in case of the attempt to upgrade an obsolete
               * meta-page that is invalid for current state of a DB,
               * e.g. after shrinking DB file */
              err != MDBX_PROBLEM) {
            ERROR("%s meta[%u], txnid %" PRIaTXN ", error %d", "updating db-format signature for", n, txnid, err);
            return err;
          }
          troika = meta_tap(env);
        }
      }
    }
  } /* lck exclusive, lck_rc == MDBX_RESULT_TRUE */

  //---------------------------------------------------- setup madvise/readahead
  if (allocated_aligned2os_bytes < env->dxb_mmap.current) {
#if defined(MADV_REMOVE)
    if (lck_rc && (env->flags & MDBX_WRITEMAP) != 0 &&
        /* not recovery mode */ env->stuck_meta < 0) {
      NOTICE("open-MADV_%s %u..%u", "REMOVE (deallocate file space)", env->lck->discarded_tail.weak,
             bytes2pgno(env, env->dxb_mmap.current));
      err = madvise(ptr_disp(env->dxb_mmap.base, allocated_aligned2os_bytes),
                    env->dxb_mmap.current - allocated_aligned2os_bytes, MADV_REMOVE)
                ? ignore_enosys_and_eagain(errno)
                : MDBX_SUCCESS;
      if (unlikely(MDBX_IS_ERROR(err)))
        return LOG_IFERR(err);
    }
#endif /* MADV_REMOVE */
#if defined(MADV_DONTNEED)
    NOTICE("open-MADV_%s %u..%u", "DONTNEED", env->lck->discarded_tail.weak, bytes2pgno(env, env->dxb_mmap.current));
    err = madvise(ptr_disp(env->dxb_mmap.base, allocated_aligned2os_bytes),
                  env->dxb_mmap.current - allocated_aligned2os_bytes, MADV_DONTNEED)
              ? ignore_enosys_and_eagain(errno)
              : MDBX_SUCCESS;
    if (unlikely(MDBX_IS_ERROR(err)))
      return LOG_IFERR(err);
#elif defined(POSIX_MADV_DONTNEED)
    err = ignore_enosys(posix_madvise(ptr_disp(env->dxb_mmap.base, allocated_aligned2os_bytes),
                                      env->dxb_mmap.current - allocated_aligned2os_bytes, POSIX_MADV_DONTNEED));
    if (unlikely(MDBX_IS_ERROR(err)))
      return LOG_IFERR(err);
#elif defined(POSIX_FADV_DONTNEED)
    err = ignore_enosys(posix_fadvise(env->lazy_fd, allocated_aligned2os_bytes,
                                      env->dxb_mmap.current - allocated_aligned2os_bytes, POSIX_FADV_DONTNEED));
    if (unlikely(MDBX_IS_ERROR(err)))
      return LOG_IFERR(err);
#endif /* MADV_DONTNEED */
  }

  err = dxb_set_readahead(env, bytes2pgno(env, allocated_bytes), readahead, true);
  if (unlikely(err != MDBX_SUCCESS))
    return LOG_IFERR(err);

  return rc;
}

/* Decide whether and how far the datafile can be shrunk (issue #154).
 *
 * Two-phase: Phase A (`dxb_shrink_maybe`) is pure arithmetic and needs no
 * reader-table scan, so the common "nothing to shrink" case skips the
 * O(readers) `mvcc_snapshot_largest()` entirely. Phase B (`dxb_shrink_bottom`)
 * computes the exact bottom from the largest live snapshot.
 *
 * Both phases are based on the REAL file size (`dxb_mmap.filesize`), not on
 * `geometry.now`. This upholds the invariant "the logical geometry is lowered
 * only together with a successful un-map/fsetsize, and a deferred shrink keeps
 * the predicate true": when a shrink is deferred on Windows (a reader holds the
 * remap-lock shared, so the mapping cannot be unmapped right now), the file
 * keeps its size and the shrink is re-attempted by a later commit.
 *
 * This predicate is intentionally decoupled from the madvise `discarded_tail`
 * gate: `discarded_tail` tracks only the POSIX MADV_DONTNEED machinery and may
 * lag far behind or run ahead of the shrink threshold, so gating the datafile
 * shrink on it is incorrect.
 */
static bool dxb_shrink_maybe(const meta_t *const pending, const size_t filesize_pgno) {
  if (!pending->geometry.shrink_pv)
    return false;
  const pgno_t stockpile_gap = 3 + pending->trees.gc.height * 3;
  const pgno_t shrink_step = pv2pages(pending->geometry.shrink_pv);
  return filesize_pgno > pending->geometry.first_unallocated &&
         filesize_pgno - pending->geometry.first_unallocated > shrink_step + stockpile_gap;
}

/* Phase B: the exact shrink target ("bottom") given the largest live snapshot.
 * Returns the new `geometry.now`, or zero as the "nothing to shrink" sentinel —
 * zero is not a usable page number here (a datafile bottom is never below
 * `geometry.lower` >= NUM_METAS), so it cannot collide with a real result. */
static pgno_t dxb_shrink_bottom(const MDBX_env *env, const meta_t *const pending, const pgno_t largest_pgno,
                                const size_t filesize_pgno) {
  const pgno_t stockpile_gap = 3 + pending->trees.gc.height * 3;
  const pgno_t shrink_step = pv2pages(pending->geometry.shrink_pv);
  if (filesize_pgno <= largest_pgno || filesize_pgno - largest_pgno <= shrink_step + stockpile_gap)
    return 0;

  const pgno_t aligner = pending->geometry.grow_pv ? /* grow_step */ pv2pages(pending->geometry.grow_pv) : shrink_step;
  const pgno_t with_stockpile_gap = largest_pgno + stockpile_gap;
  const pgno_t aligned = pgno_ceil2os_pgno(env, (size_t)with_stockpile_gap + aligner - with_stockpile_gap % aligner);
  const pgno_t bottom = (aligned > pending->geometry.lower) ? aligned : pending->geometry.lower;
  return (bottom < filesize_pgno) ? bottom : 0;
}

int dxb_sync_locked(MDBX_env *env, unsigned flags, meta_t *const pending, troika_t *const troika) {
  eASSERT0(env, ((env->flags ^ flags) & MDBX_WRITEMAP) == 0);
  eASSERT0(env, pending->trees.gc.flags == MDBX_INTEGERKEY);
  eASSERT0(env, check_table_flags(pending->trees.main.flags));
  const meta_t *const meta0 = METAPAGE(env, 0);
  const meta_t *const meta1 = METAPAGE(env, 1);
  const meta_t *const meta2 = METAPAGE(env, 2);
  const meta_ptr_t head = meta_recent(env, troika);
  int rc;

  eASSERT0(env, pending < METAPAGE(env, 0) || pending > METAPAGE(env, NUM_METAS));
  eASSERT0(env, (env->flags & (MDBX_RDONLY | ENV_FATAL_ERROR)) == 0);
  eASSERT0(env, pending->geometry.first_unallocated <= pending->geometry.now);

  if (flags & ENV_UNSYNC) {
    /* Check auto-sync conditions */
    const pgno_t autosync_threshold = atomic_load32(&env->lck->autosync_threshold, mo_Relaxed);
    const uint64_t autosync_period = atomic_load64(&env->lck->autosync_period, mo_Relaxed);
    uint64_t eoos_timestamp;
    if ((autosync_threshold && atomic_load64(&env->lck->unsynced_pages, mo_Relaxed) >= autosync_threshold) ||
        (autosync_period && (eoos_timestamp = atomic_load64(&env->lck->eoos_timestamp, mo_Relaxed)) &&
         osal_monotime() - eoos_timestamp >= autosync_period))
      flags &= MDBX_WRITEMAP | txn_unmap_allowed; /* force steady */
  }

  size_t shrink = 0;
  if (flags & txn_unmap_allowed) {
    const size_t filesize_pgno = (size_t)(env->dxb_mmap.filesize >> env->ps2ln);

    /* LY: check conditions to discard unused pages via MADV_DONTNEED (POSIX
     * only). Gated by `discarded_tail`, a property of this madvise machinery
     * alone and deliberately NOT used to gate the datafile shrink below. */
    const size_t prev_discarded_pgno = atomic_load32(&env->lck->discarded_tail, mo_Relaxed);
    const bool need_discard_scan = prev_discarded_pgno >= pending->geometry.first_unallocated + env->madv_threshold;
    if (prev_discarded_pgno < pending->geometry.first_unallocated)
      env->lck->discarded_tail.weak = pending->geometry.first_unallocated;

    /* LY: check conditions to shrink the datafile; see dxb_shrink_maybe()/
     * dxb_shrink_bottom() for the rationale (real filesize, decoupled from
     * `discarded_tail`, two-phase). A deferred shrink (a reader holds the
     * remap-lock, so the mapping cannot be unmapped right now) is re-attempted
     * by a later commit because the predicate reads the real file size.
     *
     * On Windows the shrink also needs the exclusive remap-lock, which an
     * in-process reader may hold shared for an unbounded time (issue #154), so
     * the attempt can be deferred in dxb_resize(). To keep such a deferral
     * cheap, the `force steady` below is additionally gated on the steady
     * checkpoint's allocated pages being beyond the new end of the file: once a
     * shrink has been attempted (whether or not it succeeded) that checkpoint
     * is no longer beyond `bottom`, so a repeated attempt neither clears
     * MDBX_NOMETASYNC/ENV_UNSYNC again nor pays another full durable sync. */
    const bool shrink_maybe = dxb_shrink_maybe(pending, filesize_pgno);

    /* Issue #170: on Windows a shrink-through-un-map is expensive and doomed
     * while readers exist (in-process ones hold the remap-lock shared, other
     * processes keep the file mapped). Skip such an attempt entirely when it
     * is not allowed right now — the throttle/exclusivity check is cheap and
     * platform-specific: on POSIX it is a no-op that always allows. Deferring
     * here leaves the meta untouched, and since the shrink predicate above
     * reads the real file size, a later commit re-attempts it naturally. */
    const bool shrink_now = shrink_maybe && dxb_shrink_attempt_allowed(env);

    /* Both the shrink (Phase B) and the madvise discard need the largest live
     * snapshot; compute it at most once and share it between the two (issue
     * #154). In the common case neither is needed and the O(readers) scan is
     * skipped entirely. */
    pgno_t largest_pgno = 0;
    if (shrink_now || need_discard_scan) {
      largest_pgno =
          mvcc_snapshot_largest(env, (head.ptr_c->geometry.first_unallocated > pending->geometry.first_unallocated)
                                         ? head.ptr_c->geometry.first_unallocated
                                         : pending->geometry.first_unallocated);
      eASSERT0(env, largest_pgno >= NUM_METAS);
    }

    if (shrink_now) {
      const pgno_t bottom = dxb_shrink_bottom(env, pending, largest_pgno, filesize_pgno);
      if (bottom) {
        if (TROIKA_HAVE_STEADY(troika) && meta_prefer_steady(env, troika).ptr_c->geometry.first_unallocated > bottom)
          /* LY: force steady/sync-meta, but only if steady-checkpoint is present and its allocated pages are beyond the
           * new end of DXB after shrink. By doing this, we guarantee that at any outcome in the database, either the
           * former/an old/large MVCC snapshot, or a new one.
           *
           * However, if the file cannot be shrinked (for instance, due to Windows limitations), then a second attempt
           * will not require a new fsync(), since `prev_steady->geometry.first_unallocated > bottom` will be false. */
          flags &= MDBX_WRITEMAP | txn_unmap_allowed;
        shrink = filesize_pgno - bottom;
        pending->geometry.now = bottom;
        if (unlikely(head.txnid == pending->unsafe_txnid)) {
          const txnid_t txnid = safe64_txnid_next(pending->unsafe_txnid);
          NOTICE("force-forward pending-txn %" PRIaTXN " -> %" PRIaTXN, pending->unsafe_txnid, txnid);
          ENSURE_OBJ(env, !env->basal_txn || !env->txn);
          if (unlikely(txnid > MAX_TXNID)) {
            rc = MDBX_TXN_FULL;
            ERROR("txnid overflow, raise %d", rc);
            goto fail;
          }
          meta_set_txnid(env, pending, txnid);
          eASSERT1(env, coherency_check_meta(env, pending, true));
        }
      }
    }

    if (need_discard_scan) {
#if defined(ENABLE_MEMCHECK) || defined(__SANITIZE_ADDRESS__)
      const pgno_t edge = env->poison_edge;
      if (edge > largest_pgno) {
        env->poison_edge = largest_pgno;
        VALGRIND_MAKE_MEM_NOACCESS(ptr_disp(env->dxb_mmap.base, pgno2bytes(env, largest_pgno)),
                                   pgno2bytes(env, edge - largest_pgno));
        MDBX_ASAN_POISON_MEMORY_REGION(ptr_disp(env->dxb_mmap.base, pgno2bytes(env, largest_pgno)),
                                       pgno2bytes(env, edge - largest_pgno));
      }
#endif /* ENABLE_MEMCHECK || __SANITIZE_ADDRESS__ */

#if defined(MADV_DONTNEED) || defined(POSIX_MADV_DONTNEED)
      const size_t discard_edge_pgno = pgno_ceil2os_pgno(env, largest_pgno);
      if (prev_discarded_pgno >= discard_edge_pgno + env->madv_threshold) {
        const size_t prev_discarded_bytes = pgno_ceil2os_bytes(env, prev_discarded_pgno);
        const size_t discard_edge_bytes = pgno2bytes(env, discard_edge_pgno);
        /* из-за выравнивания prev_discarded_bytes и discard_edge_bytes могут быть равны */
        if (prev_discarded_bytes > discard_edge_bytes) {
          NOTICE("shrink-MADV_%s %zu..%zu", "DONTNEED", discard_edge_pgno, prev_discarded_pgno);
          munlock_after(env, discard_edge_pgno, bytes_ceil2os_bytes(env, env->dxb_mmap.current));
          const uint32_t munlocks_before = atomic_load32(&env->lck->mlcnt[1], mo_Relaxed);
#if defined(MADV_DONTNEED)
          int advise = MADV_DONTNEED;
#if defined(MADV_FREE) && 0 /* MADV_FREE works for only anonymous vma at the moment */
          if ((env->flags & MDBX_WRITEMAP) && global.linux_kernel_version > 0x04050000)
            advise = MADV_FREE;
#endif /* MADV_FREE */
          int err = madvise(ptr_disp(env->dxb_mmap.base, discard_edge_bytes), prev_discarded_bytes - discard_edge_bytes,
                            advise)
                        ? ignore_enosys_and_eagain(errno)
                        : MDBX_SUCCESS;
#else
          int err = ignore_enosys(posix_madvise(ptr_disp(env->dxb_mmap.base, discard_edge_bytes),
                                                prev_discarded_bytes - discard_edge_bytes, POSIX_MADV_DONTNEED));
#endif
          if (unlikely(MDBX_IS_ERROR(err))) {
            const uint32_t mlocks_after = atomic_load32(&env->lck->mlcnt[0], mo_Relaxed);
            if (err == MDBX_EINVAL) {
              const int severity = (mlocks_after - munlocks_before) ? MDBX_LOG_NOTICE : MDBX_LOG_WARN;
              if (LOG_ENABLED(severity))
                debug_log(severity, __func__, __LINE__,
                          "%s-madvise: ignore EINVAL (%d) since some pages maybe "
                          "locked (%u/%u mlcnt-processes)",
                          "shrink", err, mlocks_after, munlocks_before);
            } else {
              ERROR("%s-madvise(%s, %zu, +%zu), %u/%u mlcnt-processes, err %d", "shrink", "DONTNEED",
                    discard_edge_bytes, prev_discarded_bytes - discard_edge_bytes, mlocks_after, munlocks_before, err);
              return err;
            }
          } else
            env->lck->discarded_tail.weak = discard_edge_pgno;
        }
      }
#endif /* MADV_DONTNEED || POSIX_MADV_DONTNEED */
    }
  }

  /* LY: step#1 - sync previously written/updated data-pages */
  rc = MDBX_RESULT_FALSE /* carry steady */;
  const uint64_t snap_unsynced_pages = atomic_load64(&env->lck->unsynced_pages, mo_Relaxed);
  if (snap_unsynced_pages) {
    if (snap_unsynced_pages == 1 && (flags & MDBX_NOMETASYNC))
      goto skip_lastmeta_sync;

    eASSERT0(env, ((flags ^ env->flags) & MDBX_WRITEMAP) == 0);
    enum osal_syncmode_bits mode_bits = MDBX_SYNC_NONE;
    if ((flags & ENV_UNSYNC) == 0) {
      mode_bits = MDBX_SYNC_DATA;
      if (pending->geometry.first_unallocated > meta_prefer_steady(env, troika).ptr_c->geometry.now)
        mode_bits |= MDBX_SYNC_SIZE;
      if ((flags & MDBX_NOMETASYNC) == 0)
        mode_bits |= MDBX_SYNC_IODQ;
    } else if (unlikely(env->incore))
      goto skip_incore_sync;

    /* MDBX_AVOID_MSYNC (1 on Windows) routes persistence through explicit
     * writes + fsync instead of flushing the mapping, because the mapping
     * flush is expensive; honoring it here also avoids a whole-mapping
     * FlushViewOfFile on every SAFE_NOSYNC/NOMETASYNC commit. */
    if ((flags & MDBX_WRITEMAP) && !MDBX_AVOID_MSYNC)
      rc = dxb_msync(env, pending->geometry.first_unallocated, mode_bits);
    else
      rc = dxb_fsync(env, mode_bits);
    if (unlikely(rc != MDBX_SUCCESS))
      goto fail;
    rc = (flags & ENV_UNSYNC) ? MDBX_RESULT_TRUE /* carry non-steady */
                              : MDBX_RESULT_FALSE /* carry steady */;
  }
  eASSERT1(env, coherency_check_meta(env, pending, true));

  /* Steady or Weak */
  if (rc == MDBX_RESULT_FALSE /* carry steady */) {
    meta_sign_as_steady(pending);
    atomic_store64(&env->lck->eoos_timestamp, 0, mo_Relaxed);
    atomic_store64(&env->lck->unsynced_pages, 0, mo_Relaxed);
  } else {
    ASSERT(rc == MDBX_RESULT_TRUE /* carry non-steady */);
  skip_incore_sync:
    eASSERT0(env, env->lck->unsynced_pages.weak > 0);
    /* Может быть нулевым если unsynced_pages > 0 в результате спиллинга.
     * eASSERT0(env, env->lck->eoos_timestamp.weak != 0); */
    unaligned_poke_u64(4, pending->sign, DATASIGN_WEAK);
  skip_lastmeta_sync:;
  }

  const bool legal4overwrite = head.txnid == pending->unsafe_txnid &&
                               !memcmp(&head.ptr_c->trees, &pending->trees, sizeof(pending->trees)) &&
                               !memcmp(&head.ptr_c->canary, &pending->canary, sizeof(pending->canary)) &&
                               !memcmp(&head.ptr_c->geometry, &pending->geometry, sizeof(pending->geometry));
  meta_t *target = nullptr;
  if (head.txnid == pending->unsafe_txnid) {
    ENSURE_OBJ(env, legal4overwrite);
    if (!head.is_steady && meta_is_steady(pending))
      target = (meta_t *)head.ptr_c;
    else {
      NOTICE("skip update meta%" PRIaPGNO " for txn#%" PRIaTXN ", since it is already steady",
             payload2page(head.ptr_c)->pgno, head.txnid);
      return MDBX_SUCCESS;
    }
  } else {
    const unsigned troika_tail = troika->tail_and_flags & 3;
    ENSURE_OBJ(env, troika_tail < NUM_METAS && troika_tail != troika->recent && troika_tail != troika->prefer_steady);
    target = (meta_t *)meta_tail(env, troika).ptr_c;
  }

  /* LY: step#2 - update meta-page. */
  DEBUG("writing meta%" PRIaPGNO " = root %" PRIaPGNO "/%" PRIaPGNO ", geo %" PRIaPGNO "/%" PRIaPGNO "-%" PRIaPGNO
        "/%" PRIaPGNO " +%u -%u, txn_id %" PRIaTXN ", %s",
        payload2page(target)->pgno, pending->trees.main.root, pending->trees.gc.root, pending->geometry.lower,
        pending->geometry.first_unallocated, pending->geometry.now, pending->geometry.upper,
        pv2pages(pending->geometry.grow_pv), pv2pages(pending->geometry.shrink_pv), pending->unsafe_txnid,
        durable_caption(pending));

  DEBUG("meta0: %s, %s, txn_id %" PRIaTXN ", root %" PRIaPGNO "/%" PRIaPGNO,
        (meta0 == head.ptr_c) ? "head"
        : (meta0 == target)   ? "tail"
                              : "stay",
        durable_caption(meta0), constmeta_txnid(meta0), meta0->trees.main.root, meta0->trees.gc.root);
  DEBUG("meta1: %s, %s, txn_id %" PRIaTXN ", root %" PRIaPGNO "/%" PRIaPGNO,
        (meta1 == head.ptr_c) ? "head"
        : (meta1 == target)   ? "tail"
                              : "stay",
        durable_caption(meta1), constmeta_txnid(meta1), meta1->trees.main.root, meta1->trees.gc.root);
  DEBUG("meta2: %s, %s, txn_id %" PRIaTXN ", root %" PRIaPGNO "/%" PRIaPGNO,
        (meta2 == head.ptr_c) ? "head"
        : (meta2 == target)   ? "tail"
                              : "stay",
        durable_caption(meta2), constmeta_txnid(meta2), meta2->trees.main.root, meta2->trees.gc.root);

  eASSERT1(env, pending->unsafe_txnid != constmeta_txnid(meta0) || (meta_is_steady(pending) && !meta_is_steady(meta0)));
  eASSERT1(env, pending->unsafe_txnid != constmeta_txnid(meta1) || (meta_is_steady(pending) && !meta_is_steady(meta1)));
  eASSERT1(env, pending->unsafe_txnid != constmeta_txnid(meta2) || (meta_is_steady(pending) && !meta_is_steady(meta2)));

  eASSERT0(env, ((env->flags ^ flags) & MDBX_WRITEMAP) == 0);
  ENSURE_OBJ(env, target == head.ptr_c || constmeta_txnid(target) < pending->unsafe_txnid);
  if (flags & MDBX_WRITEMAP) {
    jitter4testing(true);
    if (likely(target != head.ptr_c)) {
      /* LY: 'invalidate' the meta. */
      meta_update_begin(env, target, pending->unsafe_txnid);
      unaligned_poke_u64(4, target->sign, DATASIGN_WEAK);
#if MDBX_CHECKING > 0 || MDBX_DEBUG > 0
      /* debug: provoke failure to catch a violators, but don't touch pagesize
       * to allow readers catch actual pagesize. */
      void *provoke_begin = &target->trees.gc.root;
      void *provoke_end = &target->sign;
      memset(provoke_begin, 0xCC, ptr_dist(provoke_end, provoke_begin));
      jitter4testing(false);
#endif /* MDBX_CHECKING > 0 || MDBX_DEBUG > 0 */

      /* LY: update info */
      target->geometry = pending->geometry;
      target->trees.gc = pending->trees.gc;
      target->trees.main = pending->trees.main;
      eASSERT0(env, target->trees.gc.flags == MDBX_INTEGERKEY);
      eASSERT0(env, check_table_flags(target->trees.main.flags));
      target->canary = pending->canary;
      memcpy(target->pages_retired, pending->pages_retired, 8);
      jitter4testing(true);

      /* LY: 'commit' the meta */
      meta_update_end(env, target, unaligned_peek_u64(4, pending->txnid_b));
      jitter4testing(true);
      eASSERT1(env, coherency_check_meta(env, target, true));
    } else {
      /* dangerous case (target == head), only sign could
       * me updated, check assertions once again */
      eASSERT0(env, legal4overwrite && !head.is_steady && meta_is_steady(pending));
    }
    memcpy(target->sign, pending->sign, 8);
    osal_flush_incoherent_cpu_writeback();
    jitter4testing(true);
    if (!env->incore) {
      if (!MDBX_AVOID_MSYNC)
        /* sync meta-pages */
        rc = dxb_msync(env, NUM_METAS, (flags & MDBX_NOMETASYNC) ? MDBX_SYNC_NONE : MDBX_SYNC_DATA | MDBX_SYNC_IODQ);
      else {
        if (MDBX_ENABLE_PGOP_STAT)
          env->lck->pgops.wops.weak += 1;
        const page_t *page = payload2page(target);
        rc = osal_pwrite(env->fd4meta, page, env->ps, ptr_dist(page, env->dxb_mmap.base));
        if (likely(rc == MDBX_SUCCESS)) {
          osal_flush_incoherent_mmap(target, sizeof(meta_t), globals.sys_pagesize);
          if ((flags & MDBX_NOMETASYNC) == 0 && env->fd4meta == env->lazy_fd)
            rc = dxb_fsync(env, MDBX_SYNC_DATA | MDBX_SYNC_IODQ);
        }
      }
      if (unlikely(rc != MDBX_SUCCESS))
        goto fail;
    }
  } else {
    if (MDBX_ENABLE_PGOP_STAT)
      env->lck->pgops.wops.weak += 1;
    const meta_t undo_meta = *target;
    eASSERT0(env, pending->trees.gc.flags == MDBX_INTEGERKEY);
    eASSERT0(env, check_table_flags(pending->trees.main.flags));
    rc = osal_pwrite(env->fd4meta, pending, sizeof(meta_t), ptr_dist(target, env->dxb_mmap.base));
    if (unlikely(rc != MDBX_SUCCESS)) {
    undo:
      DEBUG("%s", "write failed, disk error?");
      /* On a failure, the pagecache still contains the new data.
       * Try write some old data back, to prevent it from being used. */
      osal_pwrite(env->fd4meta, &undo_meta, sizeof(meta_t), ptr_dist(target, env->dxb_mmap.base));
      goto fail;
    }
    osal_flush_incoherent_mmap(target, sizeof(meta_t), globals.sys_pagesize);
    /* sync meta-pages */
    if (env->fd4meta == env->lazy_fd && !env->incore) {
      if (flags & MDBX_NOMETASYNC)
        env->lck->unsynced_pages.weak += 1;
      else {
        rc = dxb_fsync(env, MDBX_SYNC_DATA | MDBX_SYNC_IODQ);
        if (rc != MDBX_SUCCESS)
          goto undo;
      }
    }
  }

  uint64_t timestamp = 0;
  /* coverity[array_null] */
  while ("workaround for https://libmdbx.dqdkfa.ru/dead-github/issues/269") {
    rc = coherency_check_written(env, pending->unsafe_txnid, target,
                                 bytes2pgno(env, ptr_dist(target, env->dxb_mmap.base)), &timestamp);
    if (likely(rc == MDBX_SUCCESS))
      break;
    if (unlikely(rc != MDBX_RESULT_TRUE))
      goto fail;
  }

  const uint32_t sync_txnid_dist = ((flags & MDBX_NOMETASYNC) == 0)                     ? 0
                                   : ((flags & MDBX_WRITEMAP) == 0 || MDBX_AVOID_MSYNC) ? MDBX_NOMETASYNC_LAZY_FD
                                                                                        : MDBX_NOMETASYNC_LAZY_WRITEMAP;
  env->lck->meta_sync_txnid.weak = pending->txnid_a[__BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__].weak - sync_txnid_dist;

  *troika = meta_tap(env);
  for (MDBX_txn *txn = env->basal_txn; txn; txn = txn->nested)
    if (troika != &txn->wr.troika)
      txn->wr.troika = *troika;

  /* LY: shrink datafile if needed */
  if (unlikely(shrink)) {
    VERBOSE("shrink to %" PRIaPGNO " pages (-%zu)", pending->geometry.now, shrink);
    rc = dxb_resize(env, pending->geometry.first_unallocated, pending->geometry.now, pending->geometry.upper,
                    implicit_shrink);
    if (rc != MDBX_SUCCESS && rc != MDBX_EPERM && rc != MDBX_UNABLE_EXTEND_MAPSIZE)
      goto fail;
    if (rc == MDBX_SUCCESS) {
      /* The shrink actually succeeded: clear the rate-limit timestamp, so a
       * FOLLOW-UP shrink-warranted commit is not throttled just because it
       * falls inside the MDBX_opt_shrink_retry_period window (issue #217).
       * The throttle is meant for DOOMED attempts (a foreign process keeps the
       * file mapped, or the remap-lock was not obtainable); one that succeeded
       * proves there is no obstacle, so the very next warranted commit must be
       * free to shrink again immediately. The field exists only on Windows,
       * where shrink-through-un-map is the costly path the throttle protects. */
#if IS_WINDOWS
      env->shrink_last_attempt = 0;
#endif /* IS_WINDOWS */
    }
    if (rc != MDBX_SUCCESS) {
      /* The shrink was DEFERRED (issue #154): on Windows an in-process reader
       * holds the remap-lock shared, so the mapping cannot be un-mapped and the
       * file keeps its larger size for now. The meta-page above was written with
       * the reduced `geometry.now` (the logical size), while the physical file
       * size is tracked separately by `dxb_mmap.filesize`.
       *
       * Keep the runtime geometry in sync with the meta (`geo_in_bytes.now`
       * mirrors `meta.geometry.now`, the logical size), so that
       * `mdbx_env_info_ex().mi_geo.current` matches a freshly started txn's
       * `geo.now` and `mdbx_env_chk()`'s `txn->geo.now == mi_geo.current /
       * pagesize` assertion holds. The physical file (`dxb_mmap.filesize` /
       * `dxb_mmap.current`) legitimately stays larger until the shrink succeeds
       * — that transient "logical < physical" state IS the deferred shrink.
       *
       * The shrink is re-attempted by a later commit because dxb_shrink_maybe()
       * / dxb_shrink_bottom() are based on the real file size, and dxb_setup()
       * reconciles the geometry on reopen. There is no way to roll the file back
       * while readers exist, so this deferral is the regular outcome, not an
       * error. */
      env->geo_in_bytes.now = pgno2bytes(env, pending->geometry.now);
    }
    eASSERT1(env, coherency_check_meta(env, target, true));
  }

  lck_t *const lck = env->lck_mmap.lck;
  if (likely(lck))
    /* toggle oldest refresh */
    atomic_store32(&lck->rdt_refresh_flag, false, mo_Relaxed);

  return MDBX_SUCCESS;

fail:
  env->flags |= ENV_FATAL_ERROR;
  return rc;
}
