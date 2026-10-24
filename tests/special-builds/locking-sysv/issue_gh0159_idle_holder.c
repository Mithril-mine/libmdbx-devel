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

/* Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/159:
 * a database that is open in a process that has not called semop() since the
 * last one must not lose its semaphore set to a colliding open of another
 * database when the process that last called semop() has exited.
 *
 * The scenario from the issue:
 *   - P1 (child) opens A and commits a write transaction (the set owner);
 *   - P2 (parent) opens A while P1 is alive, and stays idle — no semop();
 *   - P1 exits (here: SIGTERM + wait), so the set keeps a dead owner pid
 *     while A is still open and usable;
 *   - P2 opens B with the same injected key: it must be refused, and A must
 *     keep working. The old owner-pid liveness check could not see the idle
 *     P2 and reclaimed (IPC_RMID) A's live set.
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

extern int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

static void trap_signal(int sig) {
  fprintf(stderr, "\nidle-holder-test: terminated by signal %d\n", sig);
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

static void cleanup_files(const char *const *paths, size_t count) {
  for (size_t i = 0; i < count; i++)
    unlink(paths[i]);
}

static int arm_collision(int key) {
  char reply[256], cmd[64];
  snprintf(cmd, sizeof cmd, "fault ftok_key_collide %d", key);
  return mdbx_probe_ctl(cmd, reply, sizeof reply);
}

int main(void) {
  signal(SIGABRT, trap_signal);
  signal(SIGSEGV, trap_signal);
  signal(SIGBUS, trap_signal);

  const char *db_a = "gh0159-idle-holder-a.mdbx";
  const char *db_b = "gh0159-idle-holder-b.mdbx";
  char lck_a[128], lck_b[128];
  snprintf(lck_a, sizeof lck_a, "%s-lck", db_a);
  snprintf(lck_b, sizeof lck_b, "%s-lck", db_b);
  const char *cleanup[] = {db_a, lck_a, db_b, lck_b};
  cleanup_files(cleanup, 4);

  int ready_pipe[2];
  if (pipe(ready_pipe) != 0) {
    perror("pipe");
    return 2;
  }

  /* Arm the FAULT site before fork(): the probe-bus state is inherited by the
   * child, so both processes derive the very same key for any database. */
  if (arm_collision(0x00712345) != 0) {
    fprintf(stderr, "cannot arm ftok_key_collide\n");
    return 2;
  }

  const pid_t child = fork();
  if (child < 0) {
    perror("fork");
    return 2;
  }
  if (child == 0) {
    /* P1: open A, do a write txn (sets the owner pid), keep A open and wait
     * for the parent to terminate us. */
    close(ready_pipe[0]);
    MDBX_env *env = NULL;
    if (open_db(db_a, &env) != 0)
      _exit(11);
    MDBX_txn *txn = NULL;
    if (mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS || mdbx_txn_commit(txn) != MDBX_SUCCESS)
      _exit(12);
    const char ready = 'R';
    if (write(ready_pipe[1], &ready, 1) != 1)
      _exit(13);
    for (;;)
      pause(); /* stay alive, owner pid stays ours */
  }

  close(ready_pipe[1]);
  char ready;
  if (read(ready_pipe[0], &ready, 1) != 1) {
    fprintf(stderr, "child never became ready\n");
    kill(child, SIGTERM);
    int status = 0;
    waitpid(child, &status, 0);
    cleanup_files(cleanup, 4);
    return 2;
  }
  close(ready_pipe[0]);

  /* P2: open A while P1 is alive — this attaches to the existing set without
   * owning it (no semop yet); stay idle afterwards. */
  MDBX_env *env_a = NULL;
  if (open_db(db_a, &env_a) != 0) {
    fprintf(stderr, "cannot open database A while P1 is alive\n");
    kill(child, SIGTERM);
    int status = 0;
    waitpid(child, &status, 0);
    cleanup_files(cleanup, 4);
    return 2;
  }

  /* P1 exits: the set's owner pid becomes dead while A is still open (idle). */
  kill(child, SIGTERM);
  int status = 0;
  if (waitpid(child, &status, 0) != child) {
    fprintf(stderr, "cannot reap P1\n");
    mdbx_env_close(env_a);
    cleanup_files(cleanup, 4);
    return 2;
  }

  /* Opening B under the same key must be refused: A's set carries the live
   * database, whose owner pid is dead. On the old code the colliding open of
   * B succeeded and removed A's set, breaking A. */
  MDBX_env *env_b = NULL;
  const int open_b_rc = open_db(db_b, &env_b);
  if (open_b_rc == 0) {
    mdbx_env_close(env_b);
    env_b = NULL;
    fprintf(stderr, "BUG PRESENT (issue #159): opening B succeeded while database A is open but idle\n");
  }

  /* A must still begin and commit a write transaction. */
  MDBX_txn *txn = NULL;
  int rc = mdbx_txn_begin(env_a, NULL, MDBX_TXN_READWRITE, &txn);
  if (rc == MDBX_SUCCESS)
    rc = mdbx_txn_commit(txn);
  mdbx_env_close(env_a);
  cleanup_files(cleanup, 4);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "BUG PRESENT (issue #159): database A broke after the colliding open: %d\n", rc);
    return 1;
  }
  if (open_b_rc == 0) {
    fprintf(stderr, "BUG PRESENT (issue #159): opening B was not refused even before A broke\n");
    return 1;
  }
  printf("OK: an idle-open database A survived a colliding open of B (B was refused)\n");
  return 0;
}
