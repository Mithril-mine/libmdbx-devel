/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

MDBX_INTERNAL int lck_setup(MDBX_env *env, mdbx_mode_t mode);
#if MDBX_LOCKING > MDBX_LOCKING_SYSV
MDBX_INTERNAL int lck_ipclock_stubinit(osal_ipclock_t *ipc);
MDBX_INTERNAL int lck_ipclock_destroy(osal_ipclock_t *ipc);
#endif /* MDBX_LOCKING > MDBX_LOCKING_SYSV */

/// \brief Initialization of synchronization primitives linked with MDBX_env
///   instance both in LCK-file and within the current process.
/// \param
///   global_uniqueness_flag = true - denotes that there are no other processes
///     working with DB and LCK-file. Thus the function MUST initialize
///     shared synchronization objects in memory-mapped LCK-file.
///   global_uniqueness_flag = false - denotes that at least one process is
///     already working with DB and LCK-file, including the case when DB
///     has already been opened in the current process. Thus the function
///     MUST NOT initialize shared synchronization objects in memory-mapped
///     LCK-file that are already in use.
/// \return Error code or zero on success.
MDBX_INTERNAL int lck_init(MDBX_env *env, MDBX_env *inprocess_neighbor, int global_uniqueness_flag);

/// \brief Disconnects from shared interprocess objects and destructs
///   synchronization objects linked with MDBX_env instance
///   within the current process.
/// \param
///   inprocess_neighbor = nullptr - if the current process does not have other
///     instances of MDBX_env linked with the DB being closed.
///     Thus the function MUST check for other processes working with DB or
///     LCK-file, and keep or destroy shared synchronization objects in
///     memory-mapped LCK-file depending on the result.
///   inprocess_neighbor = not-nullptr - pointer to another instance of MDBX_env
///     (anyone of there is several) working with DB or LCK-file within the
///     current process. Thus the function MUST NOT try to acquire exclusive
///     lock and/or try to destruct shared synchronization objects linked with
///     DB or LCK-file. Moreover, the implementation MUST ensure correct work
///     of other instances of MDBX_env within the current process, e.g.
///     restore POSIX-fcntl locks after the closing of file descriptors.
/// \return Error code (MDBX_PANIC) or zero on success.
MDBX_INTERNAL int lck_destroy(MDBX_env *env, MDBX_env *inprocess_neighbor, const mdbx_pid_t current_pid);

/// \brief Connects to shared interprocess locking objects and tries to acquire
///   the maximum lock level (shared if exclusive is not available)
///   Depending on implementation or/and platform (Windows) this function may
///   acquire the non-OS super-level lock (e.g. for shared synchronization
///   objects initialization), which will be downgraded to OS-exclusive or
///   shared via explicit calling of lck_downgrade().
/// \return
///   MDBX_RESULT_TRUE (-1) - if an exclusive lock was acquired and thus
///     the current process is the first and only after the last use of DB.
///   MDBX_RESULT_FALSE (0) - if a shared lock was acquired and thus
///     DB has already been opened and now is used by other processes.
///   Otherwise (not 0 and not -1) - error code.
MDBX_INTERNAL int lck_seize(MDBX_env *env);

/// \brief Downgrades the level of initially acquired lock to
///   operational level specified by argument. The reason for such downgrade:
///    - unblocking of other processes that are waiting for access, i.e.
///      if (env->flags & MDBX_EXCLUSIVE) != 0, then other processes
///      should be made aware that access is unavailable rather than
///      wait for it.
///    - freeing locks that interfere file operation (especially for Windows)
///   (env->flags & MDBX_EXCLUSIVE) == 0 - downgrade to shared lock.
///   (env->flags & MDBX_EXCLUSIVE) != 0 - downgrade to exclusive
///   operational lock.
/// \return Error code or zero on success
MDBX_INTERNAL int lck_downgrade(MDBX_env *env);

MDBX_MAYBE_UNUSED MDBX_INTERNAL int lck_upgrade(MDBX_env *env, bool dont_wait);

/// \brief Locks LCK-file or/and table of readers for (de)registering.
/// \return Error code or zero on success
MDBX_INTERNAL int lck_rdt_lock(MDBX_env *env);

/// \brief Unlocks LCK-file or/and table of readers after (de)registering.
MDBX_INTERNAL void lck_rdt_unlock(MDBX_env *env);

/// \brief Acquires write-transaction lock.
/// \return Error code or zero on success
MDBX_INTERNAL int lck_txn_lock(MDBX_env *env, bool dont_wait);

/// \brief Releases write-transaction lock..
MDBX_INTERNAL void lck_txn_unlock(MDBX_env *env);

/// \brief Whether an `MDBX_EXCLUSIVE` environment keeps the normal (fast)
///   durable i/o path on Windows, enforcing exclusivity by constant byte-range
///   locks over the data file instead of an unshareable data file handle.
///
/// Always false on non-Windows platforms (POSIX exclusive never switched the
/// i/o path) and under Wine, where `LockFileEx()` emulation is incomplete, so
/// the historical ShareMode==0 behavior is preserved. Also false for read-only
/// exclusive opens (no writable handle to hold the locks with) and for the
/// recovery mode (`stuck_meta >= 0`), where mdbx_env_turn_for_recovery()
/// re-reads the meta pages through lazy_fd after the open, which would clash
/// with a constant meta lock held via fd4meta; recovery exclusivity is already
/// guaranteed by the unshareable lock file.
/// \see https://github.com/Mithril-mine/libmdbx-devel/issues/179
static inline bool env_exclusive_fastpath(const MDBX_env *env) {
  return IS_WINDOWS && MDBX_ENABLE_EXCLUSIVE_FASTPATH && env->stuck_meta < 0 &&
         (env->flags & (MDBX_EXCLUSIVE | MDBX_RDONLY)) == MDBX_EXCLUSIVE && !mdbx_running_under_Wine();
}

/// \brief Acquires the constant byte-range locks that enforce exclusivity of
///   an `MDBX_EXCLUSIVE` environment when env_exclusive_fastpath() is true.
///   The meta region is locked through `fd4meta` and the body region through
///   the handle that writes it, because Windows byte-range locks are
///   per-handle and a region may only be written through its lock owner.
///
/// Windows-only, therefore a no-op on other platforms where `MDBX_EXCLUSIVE`
/// never switched the i/o path and the fcntl locks taken by lck_seize()
/// already provide the exclusivity.
/// \return Error code or zero on success.
#if IS_WINDOWS
MDBX_INTERNAL int windows_seize_exclusive(MDBX_env *env);
#else
#define windows_seize_exclusive(env) MDBX_SUCCESS
#endif /* Windows */

/// \brief Sets alive-flag of reader presence (indicative lock) for PID of
///   the current process. The function does no more than needed for
///   the correct working of lck_rpid_check() in other processes.
/// \return Error code or zero on success
MDBX_INTERNAL int lck_rpid_set(MDBX_env *env);

/// \brief Resets alive-flag of reader presence (indicative lock)
///   for PID of the current process. The function does no more than needed
///   for the correct working of lck_rpid_check() in other processes.
/// \return Error code or zero on success
MDBX_INTERNAL int lck_rpid_clear(MDBX_env *env);

/// \brief Checks for reading process status with the given pid with help of
///   alive-flag of presence (indicative lock) or using another way.
/// \return
///   MDBX_RESULT_TRUE (-1) - if the reader process with the given PID is alive
///     and working with DB (indicative lock is present).
///   MDBX_RESULT_FALSE (0) - if the reader process with the given PID is absent
///     or not working with DB (indicative lock is not present).
///   Otherwise (not 0 and not -1) - error code.
MDBX_INTERNAL int lck_rpid_check(MDBX_env *env, uint32_t pid);

#if MDBX_LOCKING == MDBX_LOCKING_SYSV
#define MDBX_LOCKING_SYSV_NSEM 2 /* txn и rdt */
#define MDBX_LOCKING_SYSV_NSEM_WITH_SIGNATURE (MDBX_LOCKING_SYSV_NSEM + MDBX_SYSVFTOK_SIGN_CHUNKS)
typedef struct mdbx_ftok mdbx_ftok_t;
MDBX_INTERNAL int lck_ftok(int fd, const char *pathname, mdbx_ftok_t *ft);
#endif /* MDBX_LOCKING_SYSV */
