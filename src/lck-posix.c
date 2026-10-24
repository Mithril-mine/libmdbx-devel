/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#if !(defined(_WIN32) || defined(_WIN64) || defined(_WINDOWS))

/*----------------------------------------------------------------------------*
 * POSIX/non-Windows LCK-implementation */

#include "internals.h"

#if MDBX_LOCKING == MDBX_LOCKING_SYSV
#include <sys/sem.h>
#include <sys/wait.h>
#define MDBX_LOCKING_SYSV_NSEM 2 /* txn и rdt */
#define MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE (MDBX_LOCKING_SYSV_NSEM + MDBX_SYSVFTOK_SIGN_CHUNKS)
#endif /* MDBX_LOCKING == MDBX_LOCKING_SYSV */

/* Описание реализации блокировок для POSIX & Linux:
 *
 * lck-файл отображается в память, в нём организуется таблица читателей и
 * размещаются совместно используемые posix-мьютексы (futex). Посредством
 * этих мьютексов (см struct lck_t) реализуются:
 *  - Блокировка таблицы читателей для регистрации,
 *    т.е. функции lck_rdt_lock() и lck_rdt_unlock().
 *  - Блокировка БД для пишущих транзакций,
 *    т.е. функции lck_txn_lock() и lck_txn_unlock().
 *
 * Остальной функционал реализуется отдельно посредством файловых блокировок:
 *  - Первоначальный захват БД в режиме exclusive/shared и последующий перевод
 *    в операционный режим, функции lck_seize() и lck_downgrade().
 *  - Проверка присутствие процессов-читателей,
 *    т.е. функции lck_rpid_set(), lck_rpid_clear() и lck_rpid_check().
 *
 * Для блокировки файлов используется fcntl(F_SETLK), так как:
 *  - lockf() оперирует только эксклюзивной блокировкой и требует
 *    открытия файла в RW-режиме.
 *  - flock() не гарантирует атомарности при смене блокировок
 *    и оперирует только всем файлом целиком.
 *  - Для контроля процессов-читателей используются однобайтовые
 *    range-блокировки lck-файла посредством fcntl(F_SETLK). При этом
 *    в качестве позиции используется pid процесса-читателя.
 *  - Для первоначального захвата и shared/exclusive выполняется блокировка
 *    основного файла БД и при успехе lck-файла.
 *
 * ----------------------------------------------------------------------------
 * УДЕРЖИВАЕМЫЕ БЛОКИРОВКИ В ЗАВИСИМОСТИ ОТ РЕЖИМА И СОСТОЯНИЯ
 *
 * Эксклюзивный режим без lck-файла:
 *   = заблокирован весь dxb-файл посредством F_RDLCK или F_WRLCK,
 *     в зависимости от MDBX_RDONLY.
 *
 * Не-операционный режим на время пере-инициализации и разрушения lck-файла:
 *   = F_WRLCK блокировка первого байта lck-файла, другие процессы ждут её
 *     снятия при получении F_RDLCK через F_SETLKW.
 *   - блокировки dxb-файла могут меняться до снятия эксклюзивной блокировки
 *     lck-файла:
 *       + для НЕ-эксклюзивного режима блокировка pid-байта в dxb-файле
 *         посредством F_RDLCK или F_WRLCK, в зависимости от MDBX_RDONLY.
 *       + для ЭКСКЛЮЗИВНОГО режима блокировка всего dxb-файла
 *         посредством F_RDLCK или F_WRLCK, в зависимости от MDBX_RDONLY.
 *
 * ОПЕРАЦИОННЫЙ режим с lck-файлом:
 *   = F_RDLCK блокировка первого байта lck-файла, другие процессы не могут
 *     получить F_WRLCK и таким образом видят что БД используется.
 *   + F_WRLCK блокировка pid-байта в clk-файле после первой транзакции чтения.
 *   + для НЕ-эксклюзивного режима блокировка pid-байта в dxb-файле
 *     посредством F_RDLCK или F_WRLCK, в зависимости от MDBX_RDONLY.
 *   + для ЭКСКЛЮЗИВНОГО режима блокировка pid-байта всего dxb-файла
 *     посредством F_RDLCK или F_WRLCK, в зависимости от MDBX_RDONLY.
 */

#if MDBX_USE_OFDLOCKS
static int op_setlk, op_setlkw, op_getlk;
__cold static void choice_fcntl(void) {
  ASSERT(!op_setlk && !op_setlkw && !op_getlk);
  if ((globals.runtime_flags & MDBX_DBG_LEGACY_MULTIOPEN) == 0
#if defined(__linux__) || defined(__gnu_linux__)
      && globals.linux_kernel_version > 0x030f0000 /* OFD locks are available since 3.15, but engages here
                                                      only for 3.16 and later kernels (i.e. LTS) because
                                                      of reliability reasons */
#endif                                             /* linux */
  ) {
    op_setlk = MDBX_F_OFD_SETLK;
    op_setlkw = MDBX_F_OFD_SETLKW;
    op_getlk = MDBX_F_OFD_GETLK;
    return;
  }
  op_setlk = MDBX_F_SETLK;
  op_setlkw = MDBX_F_SETLKW;
  op_getlk = MDBX_F_GETLK;
}
#else
#define op_setlk MDBX_F_SETLK
#define op_setlkw MDBX_F_SETLKW
#define op_getlk MDBX_F_GETLK
#endif /* MDBX_USE_OFDLOCKS */

static int lck_op(const mdbx_filehandle_t fd, int cmd, const int lck, const off_t offset, off_t len) {
  STATIC_ASSERT(sizeof(off_t) >= sizeof(void *) && sizeof(off_t) >= sizeof(size_t));
#if defined(__ANDROID_API__) && __ANDROID_API__ < 24
  STATIC_ASSERT_MSG((sizeof(off_t) * CHAR_BIT == MDBX_WORDBITS),
                    "The bitness of system `off_t` type is mismatch. Please "
                    "fix build and/or NDK configuration.");
#endif /* Android && API < 24 */
  ASSERT(offset >= 0 && len > 0);
  ASSERT((uint64_t)offset < (uint64_t)INT64_MAX && (uint64_t)len < (uint64_t)INT64_MAX &&
         (uint64_t)(offset + len) > (uint64_t)offset);

  ASSERT((uint64_t)offset < (uint64_t)OFF_T_MAX && (uint64_t)len <= (uint64_t)OFF_T_MAX &&
         (uint64_t)(offset + len) <= (uint64_t)OFF_T_MAX);

  ASSERT((uint64_t)((off_t)((uint64_t)offset + (uint64_t)len)) == ((uint64_t)offset + (uint64_t)len));

  jitter4testing(true);
  for (;;) {
    MDBX_STRUCT_FLOCK lock_op = {.l_type = lck, .l_whence = SEEK_SET, .l_start = offset, .l_len = len};
    STATIC_ASSERT_MSG(sizeof(off_t) <= sizeof(lock_op.l_start) && sizeof(off_t) <= sizeof(lock_op.l_len) &&
                          OFF_T_MAX == (off_t)OFF_T_MAX,
                      "Support for large/64-bit-sized files is misconfigured "
                      "for the target system and/or toolchain. "
                      "Please fix it or at least disable it completely.");
    int rc = MDBX_FCNTL(fd, cmd, &lock_op);
    jitter4testing(true);
    if (rc != -1) {
      if (cmd == op_getlk) {
        ASSERT(lock_op.l_pid != getpid());
        /* Checks reader by pid. Returns:
         *   MDBX_RESULT_TRUE   - if pid is live (reader holds a lock).
         *   MDBX_RESULT_FALSE  - if pid is dead (a lock could be placed). */
        return (lock_op.l_type == F_UNLCK) ? MDBX_RESULT_FALSE : MDBX_RESULT_TRUE;
      }
      return MDBX_SUCCESS;
    }
    rc = errno;
#if MDBX_USE_OFDLOCKS
    if (ignore_enosys_and_einval(rc) == MDBX_RESULT_TRUE &&
        (cmd == MDBX_F_OFD_SETLK || cmd == MDBX_F_OFD_SETLKW || cmd == MDBX_F_OFD_GETLK)) {
      /* fallback to non-OFD locks */
      if (cmd == MDBX_F_OFD_SETLK)
        cmd = MDBX_F_SETLK;
      else if (cmd == MDBX_F_OFD_SETLKW)
        cmd = MDBX_F_SETLKW;
      else
        cmd = MDBX_F_GETLK;
      op_setlk = MDBX_F_SETLK;
      op_setlkw = MDBX_F_SETLKW;
      op_getlk = MDBX_F_GETLK;
      continue;
    }
#endif /* MDBX_USE_OFDLOCKS */
    if (rc != EINTR || cmd == op_setlkw) {
      ASSERT(MDBX_IS_ERROR(rc));
      return rc;
    }
  }
}

static int lck_setlk_with3retries(const mdbx_filehandle_t fd, const int lck, const off_t offset, off_t len) {
  ASSERT(lck != F_UNLCK);
  int retry_left = 3;
#if defined(__ANDROID_API__)
  retry_left *= 3;
#endif /* Android */
  while (true) {
    int rc = lck_op(fd, op_setlk, lck, offset, len);
    if (!(rc == EAGAIN || rc == EACCES || rc == EBUSY || rc == EWOULDBLOCK || rc == EDEADLK) || --retry_left < 1)
      return rc;
#if defined(__ANDROID_API__)
    if (retry_left == 5 || retry_left == 3) {
      usleep(1000 * 42 / 2);
      continue;
    }
#endif /* Android */
    if (osal_yield())
      return rc;
  }
}

int osal_lockfile(mdbx_filehandle_t fd, bool wait) {
#if MDBX_USE_OFDLOCKS
  if (unlikely(op_setlk == 0))
    choice_fcntl();
#endif /* MDBX_USE_OFDLOCKS */
  return lck_op(fd, wait ? op_setlkw : op_setlk, F_WRLCK, 0, OFF_T_MAX);
}

int lck_rpid_set(MDBX_env *env) {
  ASSERT(env->lck_mmap.fd != INVALID_HANDLE_VALUE);
  ASSERT(env->pid > 0);
  if (unlikely(osal_getpid() != env->pid))
    return MDBX_PANIC;
  return lck_op(env->lck_mmap.fd, op_setlk, F_WRLCK, env->pid, 1);
}

int lck_rpid_clear(MDBX_env *env) {
  ASSERT(env->lck_mmap.fd != INVALID_HANDLE_VALUE);
  ASSERT(env->pid > 0);
  return lck_op(env->lck_mmap.fd, op_setlk, F_UNLCK, env->pid, 1);
}

int lck_rpid_check(MDBX_env *env, uint32_t pid) {
  ASSERT(env->lck_mmap.fd != INVALID_HANDLE_VALUE);
  ASSERT(pid > 0);
  return lck_op(env->lck_mmap.fd, op_getlk, F_WRLCK, pid, 1);
}

/*---------------------------------------------------------------------------*/

#if MDBX_LOCKING > MDBX_LOCKING_SYSV
int lck_ipclock_stubinit(osal_ipclock_t *ipc) {
#if MDBX_LOCKING == MDBX_LOCKING_POSIX1988
  return sem_init(ipc, false, 1) ? errno : 0;
#elif MDBX_LOCKING == MDBX_LOCKING_POSIX2001 || MDBX_LOCKING == MDBX_LOCKING_POSIX2008
  return pthread_mutex_init(ipc, nullptr);
#else
#error "FIXME"
#endif
}

int lck_ipclock_destroy(osal_ipclock_t *ipc) {
#if MDBX_LOCKING == MDBX_LOCKING_POSIX1988
  return sem_destroy(ipc) ? errno : 0;
#elif MDBX_LOCKING == MDBX_LOCKING_POSIX2001 || MDBX_LOCKING == MDBX_LOCKING_POSIX2008
  return pthread_mutex_destroy(ipc);
#else
#error "FIXME"
#endif
}
#endif /* MDBX_LOCKING > MDBX_LOCKING_SYSV */

static int check_fstat(MDBX_env *env) {
  struct stat st;

  int rc = MDBX_SUCCESS;
  if (fstat(env->lazy_fd, &st)) {
    rc = errno;
    ERROR("fstat(%s), err %d", "DXB", rc);
    return rc;
  }

  if (!S_ISREG(st.st_mode) || st.st_nlink < 1) {
#ifdef EBADFD
    rc = EBADFD;
#else
    rc = EPERM;
#endif
    ERROR("%s %s, err %d", "DXB", (st.st_nlink < 1) ? "file was removed" : "not a regular file", rc);
    return rc;
  }

  if (st.st_size < (off_t)(MDBX_MIN_PAGESIZE * NUM_METAS)) {
    VERBOSE("dxb-file is too short (%u), exclusive-lock needed", (unsigned)st.st_size);
    rc = MDBX_RESULT_TRUE;
  }

  //----------------------------------------------------------------------------

  if (fstat(env->lck_mmap.fd, &st)) {
    rc = errno;
    ERROR("fstat(%s), err %d", "LCK", rc);
    return rc;
  }

  if (!S_ISREG(st.st_mode) || st.st_nlink < 1) {
#ifdef EBADFD
    rc = EBADFD;
#else
    rc = EPERM;
#endif
    ERROR("%s %s, err %d", "LCK", (st.st_nlink < 1) ? "file was removed" : "not a regular file", rc);
    return rc;
  }

  /* Checking file size for detect the situation when we got the shared lock
   * immediately after lck_destroy(). */
  if (st.st_size < (off_t)(sizeof(lck_t) + sizeof(reader_slot_t))) {
    VERBOSE("lck-file is too short (%u), exclusive-lock needed", (unsigned)st.st_size);
    rc = MDBX_RESULT_TRUE;
  }

  return rc;
}

__cold int lck_seize(MDBX_env *env) {
  ASSERT(env->lazy_fd != INVALID_HANDLE_VALUE);
  if (unlikely(osal_getpid() != env->pid))
    return MDBX_PANIC;

  int rc = MDBX_SUCCESS;
#if defined(__linux__) || defined(__gnu_linux__)
  if (unlikely(globals.running_on_WSL1)) {
    rc = ENOLCK /* No record locks available */;
    ERROR("%s, err %u",
          "WSL1 (Windows Subsystem for Linux) is mad and trouble-full, "
          "injecting failure to avoid data loss",
          rc);
    return rc;
  }
#endif /* Linux */

#if MDBX_USE_OFDLOCKS
  if (unlikely(op_setlk == 0))
    choice_fcntl();
#endif /* MDBX_USE_OFDLOCKS */

  if (env->lck_mmap.fd == INVALID_HANDLE_VALUE) {
    /* LY: without-lck mode (e.g. exclusive or on read-only filesystem) */
    rc = lck_setlk_with3retries(env->lazy_fd, (env->flags & MDBX_RDONLY) ? F_RDLCK : F_WRLCK, 0, OFF_T_MAX);
    if (rc != MDBX_SUCCESS) {
      ERROR("%s, err %u", "without-lck", rc);
      eASSERT0(env, MDBX_IS_ERROR(rc));
      return rc;
    }
    return MDBX_RESULT_TRUE /* Done: return with exclusive locking. */;
  }

retry:
  if (rc == MDBX_RESULT_TRUE) {
    rc = lck_op(env->lck_mmap.fd, op_setlk, F_UNLCK, 0, 1);
    if (rc != MDBX_SUCCESS) {
      ERROR("%s, err %u", "unlock-before-retry", rc);
      eASSERT0(env, MDBX_IS_ERROR(rc));
      return rc;
    }
  }

  /* Firstly try to get exclusive locking.  */
  rc = lck_setlk_with3retries(env->lck_mmap.fd, F_WRLCK, 0, 1);
  if (rc == MDBX_SUCCESS) {
    rc = check_fstat(env);
    if (MDBX_IS_ERROR(rc))
      return rc;

  continue_dxb_exclusive:
    rc = lck_setlk_with3retries(env->lazy_fd, (env->flags & MDBX_RDONLY) ? F_RDLCK : F_WRLCK, 0, OFF_T_MAX);
    if (rc == MDBX_SUCCESS)
      return MDBX_RESULT_TRUE /* Done: return with exclusive locking. */;

    int err = check_fstat(env);
    if (MDBX_IS_ERROR(err))
      return err;

    /* the cause may be a collision with POSIX's file-lock recovery. */
    if (!(rc == EAGAIN || rc == EACCES || rc == EBUSY || rc == EWOULDBLOCK || rc == EDEADLK)) {
      ERROR("%s, err %u", "dxb-exclusive", rc);
      eASSERT0(env, MDBX_IS_ERROR(rc));
      return rc;
    }

    /* Fallback to lck-shared */
  } else if (!(rc == EAGAIN || rc == EACCES || rc == EBUSY || rc == EWOULDBLOCK || rc == EDEADLK)) {
    ERROR("%s, err %u", "try-exclusive", rc);
    eASSERT0(env, MDBX_IS_ERROR(rc));
    return rc;
  }

  /* Here could be one of two:
   *  - lck_destroy() from the another process was hold the lock
   *    during a destruction.
   *  - either lck_seize() from the another process was got the exclusive
   *    lock and doing initialization.
   * For distinguish these cases will use size of the lck-file later. */

  /* Wait for lck-shared now. */
  /* Here may be await during transient processes, for instance until another
   * competing process doesn't call lck_downgrade(). */
  rc = lck_op(env->lck_mmap.fd, op_setlkw, F_RDLCK, 0, 1);
  if (rc != MDBX_SUCCESS) {
    ERROR("%s, err %u", "try-shared", rc);
    eASSERT0(env, MDBX_IS_ERROR(rc));
    return rc;
  }

  rc = check_fstat(env);
  if (rc == MDBX_RESULT_TRUE)
    goto retry;
  if (rc != MDBX_SUCCESS) {
    ERROR("%s, err %u", "lck_fstat", rc);
    return rc;
  }

  /* got shared, retry exclusive */
  rc = lck_setlk_with3retries(env->lck_mmap.fd, F_WRLCK, 0, 1);
  if (rc == MDBX_SUCCESS)
    goto continue_dxb_exclusive;

  if (!(rc == EAGAIN || rc == EACCES || rc == EBUSY || rc == EWOULDBLOCK || rc == EDEADLK)) {
    ERROR("%s, err %u", "try-exclusive", rc);
    eASSERT0(env, MDBX_IS_ERROR(rc));
    return rc;
  }

  /* Lock against another process operating in without-lck or exclusive mode. */
  rc = lck_setlk_with3retries(env->lazy_fd, (env->flags & MDBX_RDONLY) ? F_RDLCK : F_WRLCK, env->pid, 1);
  if (rc != MDBX_SUCCESS) {
    ERROR("%s, err %u", "lock-against-without-lck", rc);
    eASSERT0(env, MDBX_IS_ERROR(rc));
    return rc;
  }

  /* Done: return with shared locking. */
  return MDBX_RESULT_FALSE;
}

int lck_downgrade(MDBX_env *env) {
  ASSERT(env->lck_mmap.fd != INVALID_HANDLE_VALUE);
  if (unlikely(osal_getpid() != env->pid))
    return MDBX_PANIC;

  int rc = MDBX_SUCCESS;
  if ((env->flags & MDBX_EXCLUSIVE) == 0) {
    rc = lck_op(env->lazy_fd, op_setlk, F_UNLCK, 0, env->pid);
    if (rc == MDBX_SUCCESS)
      rc = lck_op(env->lazy_fd, op_setlk, F_UNLCK, env->pid + 1, OFF_T_MAX - env->pid - 1);
  }
  if (rc == MDBX_SUCCESS)
    rc = lck_setlk_with3retries(env->lck_mmap.fd, F_RDLCK, 0, 1);
  if (unlikely(rc != 0)) {
    ERROR("%s, err %u", "lck", rc);
    ASSERT(MDBX_IS_ERROR(rc));
  }
  return rc;
}

int lck_upgrade(MDBX_env *env, bool dont_wait) {
  ASSERT(env->lck_mmap.fd != INVALID_HANDLE_VALUE);
  if (unlikely(osal_getpid() != env->pid))
    return MDBX_PANIC;

  const int cmd = dont_wait ? op_setlk : op_setlkw;
  int rc = lck_op(env->lck_mmap.fd, cmd, F_WRLCK, 0, 1);
  if (rc == MDBX_SUCCESS && (env->flags & MDBX_EXCLUSIVE) == 0) {
    rc = lck_op(env->lazy_fd, cmd, F_WRLCK, 0, env->pid);
    if (rc == MDBX_SUCCESS) {
      rc = lck_op(env->lazy_fd, cmd, F_WRLCK, env->pid + 1, OFF_T_MAX - env->pid - 1);
      if (rc != MDBX_SUCCESS && lck_setlk_with3retries(env->lazy_fd, F_UNLCK, 0, env->pid))
        rc = MDBX_PANIC;
    }
    if (rc != MDBX_SUCCESS && lck_setlk_with3retries(env->lck_mmap.fd, F_RDLCK, 0, 1))
      rc = MDBX_PANIC;
  }
  if (unlikely(rc != 0)) {
    ERROR("%s, err %u", "lck", rc);
    ASSERT(MDBX_IS_ERROR(rc));
  }
  return rc;
}

__cold int lck_destroy(MDBX_env *env, MDBX_env *inprocess_neighbor, const mdbx_pid_t current_pid) {
  eASSERT0(env, osal_getpid() == current_pid);

  int rc = MDBX_SUCCESS;
  if (current_pid != env->pid) {
    VERBOSE("%p skip drown ipc-locks after fork (pid %u -> %u)", __Wpedantic_format_voidptr(env), env->pid,
            current_pid);
  } else if (inprocess_neighbor) {
    VERBOSE("%p has inprocess-neighbor %p, skip drown ipc-locks", __Wpedantic_format_voidptr(env),
            __Wpedantic_format_voidptr(inprocess_neighbor));
  } else {
    if ((env->lck_mmap.fd == INVALID_HANDLE_VALUE ||
         /* acquire LCK exclusive */ lck_op(env->lck_mmap.fd, op_setlk, F_WRLCK, 0, OFF_T_MAX) == 0) &&
        (env->lazy_fd == INVALID_HANDLE_VALUE ||
         /* acquire DXB exclusive */ lck_op(env->lazy_fd, op_setlk, (env->flags & MDBX_RDONLY) ? F_RDLCK : F_WRLCK, 0,
                                            OFF_T_MAX) == 0)) {
      VERBOSE("%p got exclusive, drown ipc-locks", __Wpedantic_format_voidptr(env));
#if MDBX_LOCKING == MDBX_LOCKING_SYSV
      if (env->me_sysv_ipc.semid != -1) {
        VERBOSE("sysv-ipc: drown  semid %d, %s ftok-key 0x%08X", env->me_sysv_ipc.semid,
                MDBX_AVOID_FTOK ? "mdbx-flavour" : "system-libc", env->me_sysv_ipc.ft.key);
        rc = semctl(env->me_sysv_ipc.semid, MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE, IPC_RMID) ? errno : 0;
        if (rc == MDBX_SUCCESS)
          env->me_sysv_ipc.semid = -1;
        else if (rc == EIDRM || rc == EINVAL) {
          NOTICE("sysv-ipc: semctl(semid = %d, IPC_RMID) failed: %d, %s", env->me_sysv_ipc.semid, rc, strerror(rc));
          env->me_sysv_ipc.semid = -1;
        } else
          ERROR("sysv-ipc: semctl(semid = %d, IPC_RMID) failed: %d, %s", env->me_sysv_ipc.semid, rc, strerror(rc));
      }
#else
      if (env->lck_mmap.lck) {
        rc = lck_ipclock_destroy(&env->lck_mmap.lck->rdt_lock);
        if (rc == 0)
          rc = lck_ipclock_destroy(&env->lck_mmap.lck->wrt_lock);
      }
#endif /* MDBX_LOCKING */

      jitter4testing(false);
      struct stat lck_info;
      const bool lck_not_removed =
          env->lck_mmap.fd != INVALID_HANDLE_VALUE && fstat(env->lck_mmap.fd, &lck_info) == 0 && lck_info.st_nlink > 0;
      if (rc == MDBX_SUCCESS && lck_not_removed && env->lck_mmap.lck) {
        const bool synced = env->lck_mmap.lck->unsynced_pages.weak == 0;
        osal_munmap(&env->lck_mmap);
        if (synced && env->lck_mmap.fd != INVALID_HANDLE_VALUE)
          rc = ftruncate(env->lck_mmap.fd, 0) ? errno : 0;
        jitter4testing(false);
      }
    } else {
      VERBOSE("%p not got exclusive(), skip drown ipc-locks", __Wpedantic_format_voidptr(env));
    }
  }

#if MDBX_LOCKING == MDBX_LOCKING_SYSV
  env->me_sysv_ipc.semid = -1;
  env->me_sysv_ipc.ft.key = -1;
#endif /* MDBX_LOCKING */

  if (current_pid != env->pid) {
    eASSERT0(env, !inprocess_neighbor);
    NOTICE("drown env %p after-fork pid %zd -> %zd", __Wpedantic_format_voidptr(env), (size_t)env->pid,
           (size_t)current_pid);
    inprocess_neighbor = nullptr;
  }

  /* 1) POSIX's fcntl() locks (i.e. when op_setlk == F_SETLK) should be restored
   * after file was closed.
   *
   * 2) File locks would be released (by kernel) while the file-descriptors will
   * be closed. But to avoid false-positive EACCESS and EDEADLK from the kernel,
   * locks should be released here explicitly with properly order. */

  /* close dxb and restore lock */
  if (env->dsync_fd != INVALID_HANDLE_VALUE) {
    if (unlikely(close(env->dsync_fd) != 0) && rc == MDBX_SUCCESS)
      rc = errno;
    env->dsync_fd = INVALID_HANDLE_VALUE;
  }
  if (env->lazy_fd != INVALID_HANDLE_VALUE) {
    if (unlikely(close(env->lazy_fd) != 0) && rc == MDBX_SUCCESS)
      rc = errno;
    env->lazy_fd = INVALID_HANDLE_VALUE;
    if (op_setlk == F_SETLK && inprocess_neighbor && rc == MDBX_SUCCESS) {
      /* restore file-lock */
      rc = lck_op(inprocess_neighbor->lazy_fd, F_SETLKW, (inprocess_neighbor->flags & MDBX_RDONLY) ? F_RDLCK : F_WRLCK,
                  (inprocess_neighbor->flags & MDBX_EXCLUSIVE) ? 0 : inprocess_neighbor->pid,
                  (inprocess_neighbor->flags & MDBX_EXCLUSIVE) ? OFF_T_MAX : 1);
    }
  }

  /* close clk and restore locks */
  if (env->lck_mmap.fd != INVALID_HANDLE_VALUE) {
    if (unlikely(close(env->lck_mmap.fd) != 0) && rc == MDBX_SUCCESS)
      rc = errno;
    env->lck_mmap.fd = INVALID_HANDLE_VALUE;
    if (op_setlk == F_SETLK && inprocess_neighbor && rc == MDBX_SUCCESS) {
      /* restore file-locks */
      rc = lck_op(inprocess_neighbor->lck_mmap.fd, F_SETLKW, F_RDLCK, 0, 1);
      if (rc == MDBX_SUCCESS && inprocess_neighbor->registered_reader_pid)
        rc = lck_rpid_set(inprocess_neighbor);
    }
  }

  if (inprocess_neighbor && rc != MDBX_SUCCESS)
    inprocess_neighbor->flags |= ENV_FATAL_ERROR;
  return rc;
}

/*---------------------------------------------------------------------------*/

#if MDBX_LOCKING == MDBX_LOCKING_SYSV
/* Error code for a System V IPC key collision: another live database holds a
 * semaphore set under our key, so opening must fail rather than reclaim it. */
#define MDBX_FTOK_COLLISION ENOLCK

/* Decision on EEXIST from semget(key, IPC_CREAT|IPC_EXCL): whether to
 * reclaim the existing semaphore set ("hit") or to bail out with an error
 * ("flee"). The `semid` of the existing set is passed so the ownership can be
 * verified; issues #80 and #159.
 *
 * The question arises only when we hold the exclusive access to the LCK-file
 * of our database, so any existing set under our key is either our own stale
 * leftover (safe to reclaim) or a foreign collision — a live database that
 * happens to share the key (must not be touched). Liveness is not judged from
 * the semop() history (GETPID/sem_otime), which does not reflect a database
 * that is open but idle or whose last locking process has exited (issue #159);
 * instead every set created by libmdbx carries a signature in the semval of
 * its extra semaphores (see osal_ftok()). The signature is set once by SETALL
 * at set creation and never changed by semop(), so it is stable and precise:
 * a mismatch means a foreign set — flee at once, without any retry.
 *
 * semget()/semctl() are not atomic, and a freshly created set has not yet had
 * its signature written by SETALL (sem_ctime changes), so before trusting a
 * signature the set is re-checked by sem_ctime; this only guards the creation
 * race, is bounded and yields between attempts. */
static int sysv_ipc_eexist_decide(const MDBX_env *env, int semid) {
  ENSURE(!env->lck && env->lck_mmap.lck && env->lck_mmap.lck->magic_and_version == MDBX_LOCK_MAGIC &&
         env->lck_mmap.lck->os_and_format == 0 && env->lck_mmap.lck->os_and_format != MDBX_LOCK_FORMAT);

  STATIC_ASSERT(MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE ==
                MDBX_LOCKING_SYSV_NSEM + ARRAY_LENGTH(env->me_sysv_ipc.ft.signature));
  for (unsigned attempt = 0;; ++attempt) {
    struct semid_ds ds;
    if (unlikely(semctl(semid, 0, IPC_STAT, &ds) < 0)) {
      const int err = errno;
      if (err == EIDRM || err == EINVAL)
        return MDBX_RESULT_TRUE; /* the set disappeared; retry creation */
      NOTICE("sysv-ipc %s failed, key %d, semid %d: %s", "IPC_STAT", env->me_sysv_ipc.ft.key, semid, strerror(err));
      return MDBX_RESULT_FALSE;
    }

    /* A foreign format (not a set created by libmdbx with the signature
     * extension) must never be reclaimed, whatever it looks like. */
    if (unlikely(ds.sem_nsems != MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE)) {
      NOTICE("sysv-ipc %s failed, key %d, semid %d: %s", "check", env->me_sysv_ipc.ft.key, semid,
             "sem_nsems != MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE");
      return MDBX_RESULT_FALSE;
    }

    if (unlikely(attempt > 8)) {
      NOTICE("sysv-ipc %s failed, key %d, semid %d: %s", "check", env->me_sysv_ipc.ft.key, semid,
             "the set is unstable, retried too many times");
      return MDBX_RESULT_FALSE;
    }

    unsigned short val_array[MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE];
    if (unlikely(semctl(semid, MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE, GETALL, &val_array) < 0)) {
      const int err = errno;
      if (err == EIDRM || err == EINVAL)
        return MDBX_RESULT_TRUE; /* the set disappeared; retry creation */
      NOTICE("sysv-ipc %s failed, key %d, semid %d: %s", "GETALL", env->me_sysv_ipc.ft.key, semid, strerror(err));
      return MDBX_RESULT_FALSE;
    }

    struct semid_ds ds_retry;
    if (unlikely(semctl(semid, 0, IPC_STAT, &ds_retry) < 0)) {
      const int err = errno;
      if (err == EIDRM || err == EINVAL)
        return MDBX_RESULT_TRUE; /* the set disappeared; retry creation */
      NOTICE("sysv-ipc %s failed, key %d, semid %d: %s", "IPC_STAT", env->me_sysv_ipc.ft.key, semid, strerror(err));
      return MDBX_RESULT_FALSE;
    }

    /* A set created but not yet initialized by SETALL changes sem_ctime; a
     * stable sem_ctime/sem_nsems means the signature below is trustworthy. */
    if (unlikely(ds.sem_ctime != ds_retry.sem_ctime || ds.sem_nsems != ds_retry.sem_nsems)) {
      osal_yield();
      continue;
    }

    for (size_t i = 0; i < ARRAY_LENGTH(env->me_sysv_ipc.ft.signature); ++i) {
      if (env->me_sysv_ipc.ft.signature[i] != val_array[MDBX_LOCKING_SYSV_NSEM + i]) {
        NOTICE("sysv-ipc %s failed, key %d, semid %d: %s", "check", env->me_sysv_ipc.ft.key, semid,
               "signature mismatch");
        return MDBX_RESULT_FALSE; /* a foreign database's set; do not touch it */
      }
    }

    VERBOSE("sysv-ipc %s done, key %d, semid %d: %s", "check", env->me_sysv_ipc.ft.key, semid, "signature match");
    return MDBX_RESULT_TRUE;
  }
}
#endif /* MDBX_LOCKING == MDBX_LOCKING_SYSV */

__cold int lck_init(MDBX_env *env, MDBX_env *inprocess_neighbor, int global_uniqueness_flag) {
#if MDBX_LOCKING == MDBX_LOCKING_SYSV
  int rc = -1, semid = -1;
  /* don't initialize semaphores twice */
  if (!inprocess_neighbor && global_uniqueness_flag == MDBX_RESULT_TRUE) {
    struct stat st;
    if (fstat(env->lazy_fd, &st))
      return errno;
  sysv_retry_create:
    semid = semget(env->me_sysv_ipc.ft.key, MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE,
                   IPC_CREAT | IPC_EXCL | (st.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)));
    if (unlikely(semid < 0)) {
      rc = errno;
      if (rc != EEXIST)
        return rc;

      /* Existing set under our key: decide whether to reclaim ("hit") or to
       * bail out ("flee") — a live foreign database collides on the key and
       * its semaphores must not be removed. */
      semid = semget(env->me_sysv_ipc.ft.key, MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE, 0);
      if (unlikely(semid < 0)) {
        rc = errno;
        if (rc == ENOENT)
          goto sysv_retry_create;
        if (rc == EINVAL)
          goto ftok_collision;
        return rc;
      }
      if (unlikely(sysv_ipc_eexist_decide(env, semid) != MDBX_RESULT_TRUE))
        goto ftok_collision;

      if (semctl(semid, MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE, IPC_RMID)) {
        rc = errno;
        if (rc == EINVAL)
          goto ftok_collision;
        if (rc != EIDRM)
          return rc;
      }
      goto sysv_retry_create;
    }

    STATIC_ASSERT(MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE ==
                  MDBX_LOCKING_SYSV_NSEM + ARRAY_LENGTH(env->me_sysv_ipc.ft.signature));
    unsigned short val_array[MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE] = {1, 1};
    for (size_t i = 0; i < ARRAY_LENGTH(env->me_sysv_ipc.ft.signature); ++i)
      val_array[MDBX_LOCKING_SYSV_NSEM + i] = env->me_sysv_ipc.ft.signature[i];
    if (semctl(semid, MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE, SETALL, val_array))
      return errno;
  } else {
    semid = semget(env->me_sysv_ipc.ft.key, MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE, 0);
    if (unlikely(semid < 0)) {
      rc = errno;
      if (rc == EINVAL) {
      /* A semaphore set already exists, but nsems is larger than the number of semaphores in that set. */
      ftok_collision:
        ERROR("%s, return MDBX_FTOK_COLLISION (%d, %s)", "sysv-ipc ftok collision detected", MDBX_FTOK_COLLISION,
              strerror(MDBX_FTOK_COLLISION));
        return MDBX_FTOK_COLLISION; /* a live set belonging to another database; do not touch it */
      }
      return rc;
    }

    /* check read & write access */
    struct semid_ds data;
    if (semctl(semid, 0, IPC_STAT, &data) || semctl(semid, 0, IPC_SET, &data))
      return errno;
  }

  env->me_sysv_ipc.semid = semid;
  return MDBX_SUCCESS;

#elif MDBX_LOCKING == MDBX_LOCKING_FUTEX
  (void)inprocess_neighbor;
  if (global_uniqueness_flag != MDBX_RESULT_TRUE)
    return MDBX_SUCCESS;
#error "FIXME: Not implemented"
#elif MDBX_LOCKING == MDBX_LOCKING_POSIX1988

  /* don't initialize semaphores twice */
  (void)inprocess_neighbor;
  if (global_uniqueness_flag == MDBX_RESULT_TRUE) {
    if (sem_init(&env->lck_mmap.lck->rdt_lock, true, 1))
      return errno;
    if (sem_init(&env->lck_mmap.lck->wrt_lock, true, 1))
      return errno;
  }
  return MDBX_SUCCESS;

#elif MDBX_LOCKING == MDBX_LOCKING_POSIX2001 || MDBX_LOCKING == MDBX_LOCKING_POSIX2008
  if (inprocess_neighbor)
    return MDBX_SUCCESS /* don't need any initialization for mutexes
      if LCK already opened/used inside current process */
        ;

  /* FIXME: Unfortunately, there is no other reliable way but to long testing
   * on each platform. On the other hand, behavior like FreeBSD is incorrect
   * and we can expect it to be rare. Moreover, even on FreeBSD without
   * additional in-process initialization, the probability of an problem
   * occurring is vanishingly small, and the symptom is a return of EINVAL
   * while locking a mutex. In other words, in the worst case, the problem
   * results in an EINVAL error at the start of the transaction, but NOT data
   * loss, nor database corruption, nor other fatal troubles. Thus, the code
   * below I am inclined to think the workaround for erroneous platforms (like
   * FreeBSD), rather than a defect of libmdbx. */
#if defined(__FreeBSD__)
  /* seems that shared mutexes on FreeBSD required in-process initialization */
  (void)global_uniqueness_flag;
#else
  /* shared mutexes on many other platforms (including Darwin and Linux's
   * futexes) doesn't need any addition in-process initialization */
  if (global_uniqueness_flag != MDBX_RESULT_TRUE)
    return MDBX_SUCCESS;
#endif

  pthread_mutexattr_t ma;
  int rc = pthread_mutexattr_init(&ma);
  if (rc)
    return rc;

  rc = pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
  if (rc)
    goto bailout;

#if MDBX_LOCKING == MDBX_LOCKING_POSIX2008
#if defined(PTHREAD_MUTEX_ROBUST) || defined(pthread_mutexattr_setrobust)
  rc = pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
#elif defined(PTHREAD_MUTEX_ROBUST_NP) || defined(pthread_mutexattr_setrobust_np)
  rc = pthread_mutexattr_setrobust_np(&ma, PTHREAD_MUTEX_ROBUST_NP);
#elif _POSIX_THREAD_PROCESS_SHARED < 200809L
  rc = pthread_mutexattr_setrobust_np(&ma, PTHREAD_MUTEX_ROBUST_NP);
#else
  rc = pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
#endif
  if (rc)
    goto bailout;
#endif /* MDBX_LOCKING == MDBX_LOCKING_POSIX2008 */

#if defined(_POSIX_THREAD_PRIO_INHERIT) && _POSIX_THREAD_PRIO_INHERIT >= 0 && !defined(MDBX_SAFE4QEMU)
  rc = pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
  if (rc == ENOTSUP)
    rc = pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_NONE);
  if (rc && rc != ENOTSUP)
    goto bailout;
#endif /* PTHREAD_PRIO_INHERIT */

  rc = pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ERRORCHECK);
  if (rc && rc != ENOTSUP)
    goto bailout;

  rc = pthread_mutex_init(&env->lck_mmap.lck->rdt_lock, &ma);
  if (rc)
    goto bailout;
  rc = pthread_mutex_init(&env->lck_mmap.lck->wrt_lock, &ma);

bailout:
  pthread_mutexattr_destroy(&ma);
  return rc;
#else
#error "FIXME"
#endif /* MDBX_LOCKING > 0 */
}

__cold static int osal_ipclock_failed(MDBX_env *env, osal_ipclock_t *ipc, const int err) {
  int rc = err;
#if MDBX_LOCKING == MDBX_LOCKING_POSIX2008 || MDBX_LOCKING == MDBX_LOCKING_SYSV

#ifndef EOWNERDEAD
#define EOWNERDEAD MDBX_RESULT_TRUE
#endif /* EOWNERDEAD */

  if (err == EOWNERDEAD) {
    /* We own the mutex. Clean up after dead previous owner. */
    const bool rlocked = ipc == &env->lck->rdt_lock;
    rc = MDBX_SUCCESS;
    if (!rlocked) {
      if (unlikely(env->txn)) {
        /* env is hosed if the dead thread was ours */
        env->flags |= ENV_FATAL_ERROR;
        env->txn = nullptr;
        rc = MDBX_PANIC;
      }
    }
    WARNING("%clock owner died, %s", (rlocked ? 'r' : 'w'), (rc ? "this process' env is hosed" : "recovering"));

    int check_rc = mvcc_cleanup_dead(env, rlocked, nullptr);
    check_rc = (check_rc == MDBX_SUCCESS) ? MDBX_RESULT_TRUE : check_rc;

#if MDBX_LOCKING == MDBX_LOCKING_SYSV
    rc = (rc == MDBX_SUCCESS) ? check_rc : rc;
#else
#if defined(PTHREAD_MUTEX_ROBUST) || defined(pthread_mutex_consistent)
    int mreco_rc = pthread_mutex_consistent(ipc);
#elif defined(PTHREAD_MUTEX_ROBUST_NP) || defined(pthread_mutex_consistent_np)
    int mreco_rc = pthread_mutex_consistent_np(ipc);
#elif _POSIX_THREAD_PROCESS_SHARED < 200809L
    int mreco_rc = pthread_mutex_consistent_np(ipc);
#else
    int mreco_rc = pthread_mutex_consistent(ipc);
#endif
    check_rc = (mreco_rc == 0) ? check_rc : mreco_rc;

    if (unlikely(mreco_rc))
      ERROR("lock recovery failed, %s", mdbx_strerror(mreco_rc));

    rc = (rc == MDBX_SUCCESS) ? check_rc : rc;
    if (MDBX_IS_ERROR(rc))
      pthread_mutex_unlock(ipc);
#endif /* MDBX_LOCKING == MDBX_LOCKING_POSIX2008 */
    return rc;
  }
#elif MDBX_LOCKING == MDBX_LOCKING_POSIX2001
  (void)ipc;
#elif MDBX_LOCKING == MDBX_LOCKING_POSIX1988
  (void)ipc;
#elif MDBX_LOCKING == MDBX_LOCKING_FUTEX
#ifdef _MSC_VER
#pragma message("warning: TODO")
#else
#warning "TODO"
#endif
  (void)ipc;
#else
#error "FIXME"
#endif /* MDBX_LOCKING */

  ERROR("mutex (un)lock failed, %s", mdbx_strerror(err));
  if (rc != EDEADLK)
    env->flags |= ENV_FATAL_ERROR;
  return rc;
}

#if defined(__ANDROID_API__) || defined(ANDROID) || defined(BIONIC)
int osal_check_tid4bionic(void) {
  /* avoid 32-bit Bionic bug/hang with 32-bit TID */
  if (sizeof(pthread_mutex_t) < sizeof(pid_t) + sizeof(unsigned)) {
    pid_t tid = gettid();
    if (unlikely(tid > 0xffff)) {
      FATAL("Raise the ENOSYS(%d) error to avoid hang due "
            "the 32-bit Bionic/Android bug with tid/thread_id 0x%08x(%i) "
            "that don’t fit in 16 bits, see "
            "https://android.googlesource.com/platform/bionic/+/master/"
            "docs/32-bit-abi.md#is-too-small-for-large-pids",
            ENOSYS, tid, tid);
      return ENOSYS;
    }
  }
  return 0;
}
#endif /* __ANDROID_API__ || ANDROID) || BIONIC */

static int osal_ipclock_lock(MDBX_env *env, osal_ipclock_t *ipc, const bool dont_wait) {
#if MDBX_LOCKING == MDBX_LOCKING_POSIX2001 || MDBX_LOCKING == MDBX_LOCKING_POSIX2008
  int rc = osal_check_tid4bionic();
  if (likely(rc == 0))
    rc = dont_wait ? pthread_mutex_trylock(ipc) : pthread_mutex_lock(ipc);
  rc = (rc == EBUSY && dont_wait) ? MDBX_BUSY : rc;
#elif MDBX_LOCKING == MDBX_LOCKING_POSIX1988
  int rc = MDBX_SUCCESS;
  if (dont_wait) {
    if (sem_trywait(ipc)) {
      rc = errno;
      if (rc == EAGAIN)
        rc = MDBX_BUSY;
    }
  } else if (sem_wait(ipc))
    rc = errno;
#elif MDBX_LOCKING == MDBX_LOCKING_SYSV
  struct sembuf op = {
      .sem_num = (ipc != &env->lck->wrt_lock), .sem_op = -1, .sem_flg = dont_wait ? IPC_NOWAIT | SEM_UNDO : SEM_UNDO};
  int rc;
  if (semop(env->me_sysv_ipc.semid, &op, 1)) {
    rc = errno;
    if (dont_wait && rc == EAGAIN)
      rc = MDBX_BUSY;
  } else {
    rc = *ipc ? EOWNERDEAD : MDBX_SUCCESS;
    *ipc = env->pid;
  }
#else
#error "FIXME"
#endif /* MDBX_LOCKING */

  if (unlikely(rc != MDBX_SUCCESS && rc != MDBX_BUSY))
    rc = osal_ipclock_failed(env, ipc, rc);
  return rc;
}

static int osal_ipclock_unlock(MDBX_env *env, osal_ipclock_t *ipc) {
  int err = MDBX_ENOSYS;
#if MDBX_LOCKING == MDBX_LOCKING_POSIX2001 || MDBX_LOCKING == MDBX_LOCKING_POSIX2008
  err = pthread_mutex_unlock(ipc);
#elif MDBX_LOCKING == MDBX_LOCKING_POSIX1988
  err = sem_post(ipc) ? errno : MDBX_SUCCESS;
#elif MDBX_LOCKING == MDBX_LOCKING_SYSV
  if (unlikely(*ipc != (pid_t)env->pid || env->me_sysv_ipc.ft.key == -1))
    err = EPERM;
  else {
    *ipc = 0;
    struct sembuf op = {.sem_num = (ipc != &env->lck->wrt_lock), .sem_op = 1, .sem_flg = SEM_UNDO};
    err = semop(env->me_sysv_ipc.semid, &op, 1) ? errno : MDBX_SUCCESS;
  }
#else
#error "FIXME"
#endif /* MDBX_LOCKING */
  int rc = err;
  if (unlikely(rc != MDBX_SUCCESS)) {
    const mdbx_pid_t current_pid = osal_getpid();
    if (current_pid != env->pid)
      rc = MDBX_SUCCESS /* a forked child: the lock belongs to the parent */;
    if (current_pid == env->pid || LOG_ENABLED(MDBX_LOG_NOTICE))
      debug_log((current_pid == env->pid) ? MDBX_LOG_FATAL : MDBX_LOG_NOTICE, "ipc-unlock()", __LINE__,
                "failed: env %p, lck-%s %p, err %d\n", __Wpedantic_format_voidptr(env),
                (env->lck == env->lck_mmap.lck) ? "mmap" : "stub", __Wpedantic_format_voidptr(env->lck), err);
  }
  return rc;
}

int lck_rdt_lock(MDBX_env *env) {
  TRACE("%s", ">>");
  jitter4testing(true);
  int rc = osal_ipclock_lock(env, &env->lck->rdt_lock, false);
  TRACE("<< rc %d", rc);
  return rc;
}

void lck_rdt_unlock(MDBX_env *env) {
  TRACE("%s", ">>");
  int err = osal_ipclock_unlock(env, &env->lck->rdt_lock);
  TRACE("<< err %d", err);
  if (unlikely(err != MDBX_SUCCESS))
    panic_fmt(env, "%s-unlock error %d", "rdt", err);
  jitter4testing(true);
}

int lck_txn_lock(MDBX_env *env, bool dont_wait) {
  TRACE("%swait %s", dont_wait ? "dont-" : "", ">>");
  jitter4testing(true);
  const int err = osal_ipclock_lock(env, &env->lck->wrt_lock, dont_wait);
  int rc = err;
  if (likely(env->basal_txn && !MDBX_IS_ERROR(err))) {
    eASSERT0(env, !env->basal_txn->owner || err == /* если другой поток в этом-же процессе завершился
                                                     не освободив блокировку */
                                                MDBX_RESULT_TRUE);
    env->basal_txn->owner = osal_thread_self();
    rc = MDBX_SUCCESS;
  }
  TRACE("<< err %d, rc %d", err, rc);
  return rc;
}

void lck_txn_unlock(MDBX_env *env) {
  TRACE("%s", ">>");
  if (env->basal_txn) {
    eASSERT1(env, env->basal_txn->owner == osal_thread_self());
    env->basal_txn->owner = 0;
  }
  int err = osal_ipclock_unlock(env, &env->lck->wrt_lock);
  TRACE("<< err %d", err);
  if (unlikely(err != MDBX_SUCCESS))
    panic_fmt(env, "%s-unlock error %d", "wrt", err);
  jitter4testing(true);
}

#endif /* !Windows LCK-implementation */
