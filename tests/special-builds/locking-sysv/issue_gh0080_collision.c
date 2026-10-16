/* Canary for https://github.com/Mithril-mine/libmdbx-devel/issues/80 (WONTFIX):
 * the System V IPC semaphore key is derived via the system ftok(), which uses
 * only the low 16 bits of the file's inode, so two databases on the same
 * filesystem can map to the same key. On a collision the open path removes the
 * existing semaphore set (IPC_RMID) and recreates it, silently breaking the
 * locking of the other database. This test deliberately crafts such a
 * collision: it opens database A, creates a file whose inode collides with A's
 * in the ftok() key space, opens database B there and closes it, then checks
 * that A's semaphore set still exists and A still works.
 *
 * While the defect is present the check FAILS and the test is registered with
 * WILL_FAIL TRUE in CMakeLists.txt, keeping CTest green while documenting the
 * known defect. Once the defect is fixed this test starts passing and must be
 * converted into a regular regression test. */
#include "mdbx.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_PROBES 200000

/* The defect currently makes the collision path crash (assertion/abort) or
 * fail; convert such a death into a clean non-zero exit so that the CTest
 * WILL_FAIL inversion works for both "crashed" and "returned error" forms. */
static void trap_signal(int sig) {
  fprintf(stderr, "\ncanary: terminated by signal %d while exercising the #80 collision defect\n", sig);
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
  char lck_a[128], lck_b[300];
  snprintf(lck_a, sizeof lck_a, "%s-lck", db_a);
  unlink(db_a);
  unlink(lck_a);

  char probedir[128];
  snprintf(probedir, sizeof probedir, "gh0080-collision-probes-%d", (int)getpid());
  if (mkdir(probedir, 0700) != 0 && errno != EEXIST) {
    perror("mkdir(probedir)");
    return 2;
  }

  MDBX_env *env_a = NULL;
  if (open_db(db_a, &env_a) != 0) {
    fprintf(stderr, "cannot open database A\n");
    return 2;
  }
  const key_t key_a = ftok(db_a, 42);
  if (key_a == (key_t)-1) {
    fprintf(stderr, "ftok(A) failed: %s\n", strerror(errno));
    mdbx_env_close(env_a);
    return 2;
  }
  struct stat st;
  if (stat(db_a, &st) != 0) {
    fprintf(stderr, "stat(A) failed\n");
    mdbx_env_close(env_a);
    return 2;
  }
  const int target = (int)(st.st_ino & 0xffff);

  /* Probe empty files until the low 16 bits of an inode collide with A's. */
  char db_b[256];
  int found = 0;
  for (size_t i = 0; i < MAX_PROBES && !found; i++) {
    char name[256];
    snprintf(name, sizeof name, "%s/probe-%06zu", probedir, i);
    const int fd = open(name, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0)
      continue;
    close(fd);
    struct stat cs;
    if (stat(name, &cs) == 0 && (int)(cs.st_ino & 0xffff) == target) {
      snprintf(db_b, sizeof db_b, "%s", name);
      found = 1;
    }
  }
  if (!found) {
    fprintf(stderr, "inconclusive: could not craft an ftok() collision on this filesystem\n");
    mdbx_env_close(env_a);
    return 2;
  }
  snprintf(lck_b, sizeof lck_b, "%s-lck", db_b);
  unlink(lck_b);

  MDBX_env *env_b = NULL;
  if (open_db(db_b, &env_b) != 0) {
    fprintf(stderr, "cannot open database B\n");
    mdbx_env_close(env_a);
    return 2;
  }
  mdbx_env_close(env_b);
  /* B's open/close happened; A's semaphore set must have survived. */
  const int semid_a = semget(key_a, 2, 0);
  const int after_open = errno;

  MDBX_txn *txn = NULL;
  int rc = mdbx_txn_begin(env_a, NULL, MDBX_TXN_READWRITE, &txn);
  if (rc != MDBX_SUCCESS) {
    mdbx_env_close(env_a);
    cleanup_files((const char *[]){db_a, lck_a, db_b, lck_b}, 4);
    fprintf(stderr,
            "BUG PRESENT (issue #80): opening the colliding database B broke database A: "
            "semget(%d, 2, 0) -> %s, write-txn begin -> %d\n",
            (int)key_a, semid_a == -1 ? strerror(after_open) : "ok", rc);
    return 1;
  }
  rc = mdbx_txn_commit(txn);
  mdbx_env_close(env_a);
  cleanup_files((const char *[]){db_a, lck_a, db_b, lck_b}, 4);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "BUG PRESENT (issue #80): commit on database A failed after opening B: %d\n", rc);
    return 1;
  }
  if (semid_a == -1) {
    fprintf(stderr, "BUG PRESENT (issue #80): database A's semaphore set was removed by opening B\n");
    return 1;
  }
  printf("OK: database A survived opening the colliding database B\n");
  return 0;
}