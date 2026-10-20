/* Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/80:
 * the System V IPC semaphore key is derived via the system ftok(), whose
 * narrow key space lets two databases collide; on a collision the open path
 * used to remove the existing semaphore set (IPC_RMID) and recreate it,
 * silently breaking the locking of the other database. The fix refuses the
 * colliding open instead.
 *
 * The collision is generated deterministically via the probe-bus: the
 * ftok_key_collide FAULT site in osal_ftok() is armed so that both databases
 * derive the very same key. Database A is opened and exercised (a write txn
 * does a semop, making its owner pid visible), then database B is opened with
 * the same injected key; the liveness check must refuse B while A's set and
 * A itself survive.
 *
 * Requires the probe-bus build (MDBX_PROBES=ON) and MDBX_PROBES=1 at run
 * time, see tests/special-builds/locking-sysv/CMakeLists.txt. */
#include "mdbx.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int mprobe_ctl(const char *request, char *reply, size_t reply_size);

static void trap_signal(int sig) {
  fprintf(stderr, "\ncollision-test: terminated by signal %d\n", sig);
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
  for (size_t i = 0; i < count; i++) {
    unlink(paths[i]);
  }
}

int main(void) {
  signal(SIGABRT, trap_signal);
  signal(SIGSEGV, trap_signal);
  signal(SIGBUS, trap_signal);

  const char *db_a = "gh0080-collision-a.mdbx";
  const char *db_b = "gh0080-collision-b.mdbx";
  char lck_a[128], lck_b[128];
  snprintf(lck_a, sizeof lck_a, "%s-lck", db_a);
  snprintf(lck_b, sizeof lck_b, "%s-lck", db_b);
  unlink(db_a);
  unlink(lck_a);
  unlink(db_b);
  unlink(lck_b);

  char reply[256], cmd[128];

  /* Arm the FAULT site: osal_ftok() will return the same injected key for
   * every database, forcing a deterministic System V IPC key collision. */
  snprintf(cmd, sizeof cmd, "fault ftok_key_collide %d", (int)0x00712345);
  if (mprobe_ctl(cmd, reply, sizeof reply) != 0) {
    fprintf(stderr, "mprobe_ctl(arm ftok_key_collide) failed\n");
    return 2;
  }

  MDBX_env *env_a = NULL;
  if (open_db(db_a, &env_a) != 0) {
    fprintf(stderr, "cannot open database A\n");
    mprobe_ctl("fault ftok_key_collide none", reply, sizeof reply);
    return 2;
  }
  /* Make A an *active* database: a write transaction performs a semop() on
   * the set, so its owner pid (GETPID) becomes visible to a colliding open. */
  MDBX_txn *txn_a = NULL;
  if (mdbx_txn_begin(env_a, NULL, MDBX_TXN_READWRITE, &txn_a) != MDBX_SUCCESS ||
      mdbx_txn_commit(txn_a) != MDBX_SUCCESS) {
    fprintf(stderr, "cannot write to database A\n");
    mdbx_env_close(env_a);
    mprobe_ctl("fault ftok_key_collide none", reply, sizeof reply);
    return 2;
  }

  /* Opening B under the same injected key must be refused: A's set is live. */
  MDBX_env *env_b = NULL;
  const int open_b_rc = open_db(db_b, &env_b);
  if (open_b_rc == 0) {
    mdbx_env_close(env_b);
    mdbx_env_close(env_a);
    cleanup_files((const char *[]){db_a, lck_a, db_b, lck_b}, 4);
    mprobe_ctl("fault ftok_key_collide none", reply, sizeof reply);
    fprintf(stderr, "BUG PRESENT (issue #80): opening B succeeded despite the live colliding database A\n");
    return 1;
  }

  /* A must still begin and commit a write transaction (which uses the set). */
  MDBX_txn *txn = NULL;
  int rc = mdbx_txn_begin(env_a, NULL, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS) {
    mdbx_env_close(env_a);
    cleanup_files((const char *[]){db_a, lck_a, db_b, lck_b}, 4);
    mprobe_ctl("fault ftok_key_collide none", reply, sizeof reply);
    fprintf(stderr, "BUG PRESENT (issue #80): opening the colliding database B broke database A: %d\n", rc);
    return 1;
  }
  rc = mdbx_txn_commit(txn);
  mdbx_env_close(env_a);
  cleanup_files((const char *[]){db_a, lck_a, db_b, lck_b}, 4);
  mprobe_ctl("fault ftok_key_collide none", reply, sizeof reply);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "BUG PRESENT (issue #80): commit on database A failed after the collision: %d\n", rc);
    return 1;
  }
  printf("OK: database A survived the colliding database B (B was refused)\n");
  return 0;
}
