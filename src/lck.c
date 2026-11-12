/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

__cold static int lck_setup_locked(MDBX_env *env, int lck_seize_rc) {
  /* The seize is done by the caller (lck_setup) before this rthc-locked section:
   * the seize may wait for the lck, which must not happen with rthc_lock() held. */

  if (env->lck_mmap.fd == INVALID_HANDLE_VALUE) {
    env->lck = lckless_stub(env);
    lck_options_init_defaults(env, env->lck);
    env->max_readers = UINT_MAX;
    DEBUG("lck-setup:%s%s%s", " lck-less", (env->flags & MDBX_RDONLY) ? " readonly" : "",
          (lck_seize_rc == MDBX_RESULT_TRUE) ? " exclusive" : " cooperative");
    return lck_seize_rc;
  }

  DEBUG("lck-setup:%s%s%s", " with-lck", (env->flags & MDBX_RDONLY) ? " readonly" : "",
        (lck_seize_rc == MDBX_RESULT_TRUE) ? " exclusive" : " cooperative");

  /* the lck-fileid has been captured early by the pathname stat in env_open() and re-verified
   * by the opened descriptor in lck_setup() */
  MDBX_env *inprocess_neighbor = nullptr;
  int err = rthc_neighbor_check(env, &inprocess_neighbor);
  if (unlikely(err != MDBX_SUCCESS))
    return err;
  if (inprocess_neighbor) {
    if ((globals.runtime_flags & MDBX_DBG_LEGACY_MULTIOPEN) == 0 || (inprocess_neighbor->flags & MDBX_EXCLUSIVE) != 0)
      return MDBX_BUSY;
    if (lck_seize_rc == MDBX_RESULT_TRUE) {
      err = lck_downgrade(env);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      lck_seize_rc = MDBX_RESULT_FALSE;
    }
  }

  uint64_t size = 0;
  err = osal_filesize(env->lck_mmap.fd, &size);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  if (lck_seize_rc == MDBX_RESULT_TRUE) {
    size = ceil_powerof2(env->max_readers * sizeof(reader_slot_t) + sizeof(lck_t), globals.sys_pagesize);
    jitter4testing(false);
  } else {
    if (env->flags & MDBX_EXCLUSIVE)
      return MDBX_BUSY;
    if (size > INT_MAX || (size & (globals.sys_pagesize - 1)) != 0 || size < globals.sys_pagesize) {
      ERROR("lck-file has invalid size %" PRIu64 " bytes", size);
      return MDBX_PROBLEM;
    }
  }

  const size_t maxreaders = ((size_t)size - sizeof(lck_t)) / sizeof(reader_slot_t);
  if (maxreaders < 4) {
    ERROR("lck-size too small (up to %" PRIuPTR " readers)", maxreaders);
    return MDBX_PROBLEM;
  }
  env->max_readers = (maxreaders <= MDBX_READERS_LIMIT) ? (unsigned)maxreaders : (unsigned)MDBX_READERS_LIMIT;

  err = osal_mmap((env->flags & MDBX_EXCLUSIVE) | MDBX_WRITEMAP, &env->lck_mmap, (size_t)size, (size_t)size,
                  lck_seize_rc ? MMAP_OPTION_SETLENGTH | MMAP_OPTION_SEMAPHORE : MMAP_OPTION_SEMAPHORE,
                  env->pathname.lck);
  if (unlikely(err != MDBX_SUCCESS))
    return err;

#ifdef MADV_DODUMP
  err = madvise(env->lck_mmap.lck, size, MADV_DODUMP) ? ignore_enosys_and_eagain(errno) : MDBX_SUCCESS;
  if (unlikely(MDBX_IS_ERROR(err)))
    return err;
#endif /* MADV_DODUMP */

#ifdef MADV_WILLNEED
  err = madvise(env->lck_mmap.lck, size, MADV_WILLNEED) ? ignore_enosys_and_eagain(errno) : MDBX_SUCCESS;
  if (unlikely(MDBX_IS_ERROR(err)))
    return err;
#elif defined(POSIX_MADV_WILLNEED)
  err = ignore_enosys(posix_madvise(env->lck_mmap.lck, size, POSIX_MADV_WILLNEED));
  if (unlikely(MDBX_IS_ERROR(err)))
    return err;
#endif /* MADV_WILLNEED */

  lck_t *lck = env->lck_mmap.lck;
  if (lck_seize_rc == MDBX_RESULT_TRUE) {
    /* If we succeed got exclusive lock, then nobody is using the lock region and we should initialize LCK-file. */
    memset(lck, 0, (size_t)size);
    jitter4testing(false);
    lck->magic_and_version = MDBX_LOCK_MAGIC;
  } else {
    if (lck->magic_and_version != MDBX_LOCK_MAGIC) {
      const bool invalid = (lck->magic_and_version >> 8) != MDBX_MAGIC;
      ERROR("lock region has %s", invalid ? "invalid magic"
                                          : "incompatible version (only applications with nearly or the "
                                            "same versions of libmdbx can share the same database)");
      return invalid ? MDBX_INVALID : MDBX_VERSION_MISMATCH;
    }
    if (lck->os_and_format != MDBX_LOCK_FORMAT) {
      ERROR("lock region has os/format signature 0x%" PRIx32 ", expected 0x%" PRIx32, lck->os_and_format,
            MDBX_LOCK_FORMAT);
      return MDBX_VERSION_MISMATCH;
    }
  }

  err = lck_init(env, inprocess_neighbor, lck_seize_rc);
  if (unlikely(err != MDBX_SUCCESS)) {
    eASSERT0(env, MDBX_IS_ERROR(err));
    return err;
  }

  if (lck_seize_rc == MDBX_RESULT_TRUE) {
    /* finalyze LCK-file initialization */
    lck->os_and_format = MDBX_LOCK_FORMAT;
    lck_options_init_defaults(env, lck);
    lck->pgops.wops.weak = 1;
    err = osal_msync(&env->lck_mmap, (size_t)size, MDBX_SYNC_DATA | MDBX_SYNC_SIZE);
    if (unlikely(err != MDBX_SUCCESS)) {
      ERROR("initial-%s for lck-file failed, err %d", "msync/fsync", err);
      eASSERT0(env, MDBX_IS_ERROR(err));
      return err;
    }
  }

  env->lck = lck;
  eASSERT0(env, !MDBX_IS_ERROR(lck_seize_rc));
  return lck_seize_rc;
}

__cold int lck_setup(MDBX_env *env, mdbx_mode_t mode) {
  eASSERT0(env, env->lazy_fd != INVALID_HANDLE_VALUE);
  eASSERT0(env, env->lck_mmap.fd == INVALID_HANDLE_VALUE);

  /* The rthc-registration is performed first; on POSIX also the neighbors-check by the lck-file's
   * pathname stat, still BEFORE the lck-file is opened: closing any descriptor of the lck-file
   * releases all classic (non-OFD) fcntl-locks of the whole process, thus a refused duplicate-open
   * must not open/close the lck-file at all. On Windows the check is performed after
   * the files are opened (there is no classic-lock loss on descriptor close). The neighbors are
   * detected by the file identities, see rthc_neighbor_check(). */
  rthc_lock();
  int err = rthc_register(env);
#if !IS_WINDOWS
  if (likely(err == MDBX_SUCCESS)) {
    /* the pending lck-fileid has been formed early by the pathname stat in env_open() */
    MDBX_env *inprocess_neighbor = nullptr;
    err = rthc_neighbor_check(env, &inprocess_neighbor);
    if (err == MDBX_SUCCESS && inprocess_neighbor && (globals.runtime_flags & MDBX_DBG_LEGACY_MULTIOPEN) == 0)
      err = MDBX_BUSY;
  }
#endif /* !Windows */
  rthc_unlock();
  if (unlikely(err != MDBX_SUCCESS))
    return err;

  mdbx_filehandle_t lck_fd = INVALID_HANDLE_VALUE;
  err = osal_openfile(MDBX_OPEN_LCK, env, env->pathname.lck, &lck_fd, mode);
  /* Test-only fault site: deterministically force the lck-open to fail with
   * EACCES, turning the environment into lckless mode regardless of the
   * filesystem/permissions (instead of relying on a read-only lock file or a
   * chmod side-effect). Necessary to verify that the meta-pages verdict does
   * not depend on the lock file being present. (issue #215) */
  MPROBE_FAULT(lck_setup_eacces, err);
  if (err != MDBX_SUCCESS) {
    switch (err) {
    case MDBX_EACCESS:
    case MDBX_EPERM:
      if (F_ISSET(env->flags, MDBX_RDONLY | MDBX_EXCLUSIVE))
        break;
      __fallthrough /* fall through */;
    case MDBX_ENOFILE:
    case MDBX_EROFS:
      if (env->flags & MDBX_RDONLY) {
        /* ENSURE the file system is read-only */
        int err_rofs = osal_check_fs_rdonly(env->lazy_fd, env->pathname.lck, err);
        if (err_rofs == MDBX_SUCCESS ||
            /* ignore ERROR_NOT_SUPPORTED for exclusive mode */
            (err_rofs == MDBX_ENOSYS && (env->flags & MDBX_EXCLUSIVE)))
          break;
        if (err_rofs != MDBX_ENOSYS)
          err = err_rofs;
      }
      __fallthrough /* fall through */;
    default:
      ERROR("unable to open lck-file %" MDBX_PRIsPATH ", env-flags 0x%X, err %d", env->pathname.lck, env->flags, err);
      return err;
    }

    /* LY: without-lck mode (e.g. exclusive or on read-only filesystem) */
    env->lck_mmap.fd = INVALID_HANDLE_VALUE;
    /* and there is nothing to match the neighbors by */
    env->lck_fileid.a = env->lck_fileid.b = 0;
    NOTICE("continue %" MDBX_PRIsPATH " within without-lck mode, env-flags 0x%X, lck-error %d", env->pathname.dxb,
           env->flags, err);
  } else {
    bool should_check_neighbors = IS_WINDOWS;
    const osal_fileid_t previous = env->lck_fileid;
    err = osal_fileid_by_fd(lck_fd, &env->lck_fileid);
    if (unlikely(err != MDBX_SUCCESS)) {
      osal_closefile(lck_fd);
      return err;
    }
    /* Re-verify the actuality of the lck-fileid captured early by the pathname stat
     * (a concurrent rename/replace could swap the file in-between), and publish the fileid
     * before the descriptor, separated by the producer fence: any opener that sees the descriptor
     * will see the matching fileid after the acquire barrier in the neighbors-check. */
    if (osal_fileid_is_set(&previous) && !IS_WINDOWS && !osal_fileid_eq(&env->lck_fileid, &previous)) {
      WARNING("%s", "the lck-file has been replaced after its identity was captured, using the opened one");
      should_check_neighbors = true;
    }
    osal_fence_producer();
    env->lck_mmap.fd = lck_fd;

    if (should_check_neighbors) {
      /* re-check the neighbors with the updated identity */
      MDBX_env *inprocess_neighbor = nullptr;
      rthc_lock();
      err = rthc_neighbor_check(env, &inprocess_neighbor);
      if (err == MDBX_SUCCESS && inprocess_neighbor &&
          ((globals.runtime_flags & MDBX_DBG_LEGACY_MULTIOPEN) == 0 ||
           (inprocess_neighbor->flags & MDBX_EXCLUSIVE) != 0))
        err = MDBX_BUSY;
      rthc_unlock();
      if (unlikely(err != MDBX_SUCCESS))
        return err;
    }
  }

  /* The seize is performed with rthc_lock() released: it may wait for the lck
   * exclusively held by a sibling thread, which cannot complete its failing open and release
   * the lck until rthc_lock() is released. */
  const int lck_seize_rc = lck_seize(env);
  if (unlikely(MDBX_IS_ERROR(lck_seize_rc)))
    return lck_seize_rc;

  rthc_lock();
  err = lck_setup_locked(env, lck_seize_rc);
  rthc_unlock();
  return err;
}

void env_clear_incore_cache(const MDBX_env *const env) {
  memset(env->lck->mincore_cache.begin, -1, sizeof(env->lck->mincore_cache.begin));
}
