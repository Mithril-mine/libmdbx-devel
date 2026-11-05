/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026
/// \author Andrea Lanfranchi (https://github.com/AndreaLanfranchi)

/* Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/323:
 * with the System V (MDBX_LOCKING=5) or POSIX-1988 (MDBX_LOCKING=1988)
 * semaphore locking, a signal delivered to a thread that waits for the write
 * lock interrupted semop()/sem_wait() with EINTR, and osal_ipclock_lock()
 * treated that as a broken lock: the waiter got EINTR, the environment became
 * fatal and the lock holder's commit returned MDBX_PANIC. The fix repeats the
 * wait on EINTR.
 *
 * The main thread holds a write transaction; a second thread starts a write
 * transaction and blocks on the write lock; the main thread keeps sending it
 * SIGUSR1 (handler without SA_RESTART) for a while, then commits. The commit,
 * the waiter's transaction and a later read transaction must all succeed.
 *
 * Built by tests/special-builds/locking-sysv and
 * tests/special-builds/locking-posix1988. */
#include "mdbx.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char db_path[] = "gh0323-eintr.mdbx";
static MDBX_env *env;
static volatile sig_atomic_t signals_handled, waiter_started;

static void on_signal(int sig) {
  (void)sig;
  signals_handled += 1;
}

static void *waiter(void *arg) {
  MDBX_txn *txn = NULL;
  waiter_started = 1;
  /* blocks on the write lock while main() holds a write transaction */
  int rc = mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  *(int *)arg = rc;
  return NULL;
}

int main(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = on_signal;
  sa.sa_flags = 0 /* no SA_RESTART: sem_wait() returns EINTR on Linux too */;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGUSR1, &sa, NULL)) {
    perror("sigaction");
    return 2;
  }

  mdbx_env_delete(db_path, MDBX_ENV_JUST_DELETE);
  int rc = mdbx_env_create(&env);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_env_open(env, db_path, MDBX_NOSUBDIR, 0644);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "cannot open the database: %d (%s)\n", rc, mdbx_strerror(rc));
    if (env)
      mdbx_env_close(env);
    mdbx_env_delete(db_path, MDBX_ENV_JUST_DELETE);
    return 2;
  }

  MDBX_txn *txn = NULL;
  rc = mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "cannot begin the write transaction: %d (%s)\n", rc, mdbx_strerror(rc));
    mdbx_env_close(env);
    mdbx_env_delete(db_path, MDBX_ENV_JUST_DELETE);
    return 2;
  }

  int waiter_rc = -1;
  pthread_t thread;
  if (pthread_create(&thread, NULL, waiter, &waiter_rc)) {
    perror("pthread_create");
    mdbx_txn_abort(txn);
    mdbx_env_close(env);
    mdbx_env_delete(db_path, MDBX_ENV_JUST_DELETE);
    return 2;
  }
  /* signal the waiter repeatedly once it is about to block, so that at least
   * one signal arrives while it waits for the write lock */
  for (int i = 0; !waiter_started && i < 10000; ++i)
    usleep(1000);
  for (int i = 0; i < 10; ++i) {
    usleep(50 * 1000);
    pthread_kill(thread, SIGUSR1);
  }
  usleep(50 * 1000);

  const int commit_rc = mdbx_txn_commit(txn);
  pthread_join(thread, NULL);
  MDBX_txn *ro = NULL;
  const int ro_rc = mdbx_txn_begin(env, NULL, MDBX_TXN_RDONLY, &ro);
  if (ro_rc == MDBX_SUCCESS)
    mdbx_txn_abort(ro);
  mdbx_env_close(env);
  mdbx_env_delete(db_path, MDBX_ENV_JUST_DELETE);

  printf("signals handled: %d; commit -> %d (%s); waiter -> %d (%s); read txn -> %d (%s)\n", (int)signals_handled,
         commit_rc, mdbx_strerror(commit_rc), waiter_rc, mdbx_strerror(waiter_rc), ro_rc, mdbx_strerror(ro_rc));
  if (signals_handled == 0) {
    fprintf(stderr, "no signal was handled, the test proves nothing\n");
    return 2;
  }
  if (commit_rc != MDBX_SUCCESS || waiter_rc != MDBX_SUCCESS || ro_rc != MDBX_SUCCESS) {
    fprintf(stderr, "BUG PRESENT (gh0323): a signal during the lock wait broke the environment\n");
    return 1;
  }
  return 0;
}
