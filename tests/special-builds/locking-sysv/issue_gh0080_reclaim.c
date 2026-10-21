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

/* Regression test for the reclaim path of https://github.com/Mithril-mine/libmdbx-devel/issues/80:
 * when the semaphore set under our key belongs to a crashed process (its last
 * semop() owner pid is dead), the open path must reclaim it and proceed —
 * refusing to open would be a denial of service for the legitimate reopen of a
 * database after an unclean shutdown.
 *
 * A child process opens the database, performs a write transaction (a semop()
 * records the child's pid as the set owner) and then _exit()s WITHOUT closing
 * the environment, so its semaphore set is left behind with a dead owner pid.
 * The parent then opens the same database: the liveness check must see the
 * dead owner, reclaim the stale set, and the open must succeed.
 *
 * Requires the probe-bus build (MDBX_PROBES=ON) and MDBX_PROBES=1 at run
 * time, see tests/special-builds/locking-sysv/CMakeLists.txt. */
#include "mdbx.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void trap_signal(int sig) {
  fprintf(stderr, "\nreclaim-test: terminated by signal %d\n", sig);
  _exit(1);
}

static int open_db(const char *path, MDBX_env **env_out) {
  MDBX_env *env = NULL;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return -1;
  if (mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, 4096) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return -1;
  }
  if (mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return -1;
  }
  *env_out = env;
  return 0;
}

int main(void) {
  signal(SIGABRT, trap_signal);
  signal(SIGSEGV, trap_signal);
  signal(SIGBUS, trap_signal);

  const char *db = "gh0080-reclaim.mdbx";
  char lck[128];
  snprintf(lck, sizeof lck, "%s-lck", db);
  unlink(db);
  unlink(lck);

  const pid_t child = fork();
  if (child < 0) {
    perror("fork");
    return 2;
  }
  if (child == 0) {
    /* Child: open, do a write txn (semop records our pid), then die WITHOUT
     * closing the env, leaving the semaphore set with a dead owner pid. */
    MDBX_env *env = NULL;
    if (open_db(db, &env) != 0)
      _exit(10);
    MDBX_txn *txn = NULL;
    if (mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS ||
        mdbx_txn_commit(txn) != MDBX_SUCCESS)
      _exit(11);
    _exit(0); /* no mdbx_env_close(): the set is left behind */
  }

  int status = 0;
  if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "child failed (status %d)\n", status);
    unlink(db);
    unlink(lck);
    return 2;
  }

  /* Parent: the stale set (dead owner) must be reclaimed and the open succeed. */
  MDBX_env *env = NULL;
  if (open_db(db, &env) != 0) {
    unlink(db);
    unlink(lck);
    fprintf(stderr, "BUG PRESENT (issue #80): reopen after a crashed owner was refused (stale set not reclaimed)\n");
    return 1;
  }
  MDBX_txn *txn = NULL;
  int rc = mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  mdbx_env_close(env);
  unlink(db);
  unlink(lck);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "BUG PRESENT (issue #80): reopen after a crashed owner failed to work: %d\n", rc);
    return 1;
  }
  printf("OK: stale semaphore set with a dead owner was reclaimed and the database reopened\n");
  return 0;
}
