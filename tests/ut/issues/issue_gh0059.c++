/* issue #59 — on classic-fcntl builds (MDBX_USE_OFDLOCKS=0) a failed second
 * in-process open of the same database dropped the first (live) handle's
 * POSIX file locks: the failed open's teardown closed descriptors and
 * released all of the process's fcntl locks on the lock file, and the
 * neighbour-restore in lck_destroy() was skipped because rthc_drown()
 * discovered the neighbour only when the lock region was already mapped.
 * Another process could then seize exclusive access under the live handle.
 *
 * Regression: after the failed second open, a forked child probing with
 * MDBX_EXCLUSIVE|MDBX_RDONLY must still get MDBX_BUSY (locks intact). */
#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHK(expr)                                                                                                      \
  do {                                                                                                                 \
    int err_ = (expr);                                                                                                 \
    if (err_ != MDBX_SUCCESS) {                                                                                        \
      fprintf(stderr, "%s:%d: %s -> %s (%d)\n", __FILE__, __LINE__, #expr, mdbx_strerror(err_), err_);                 \
      exit(2);                                                                                                         \
    }                                                                                                                  \
  } while (0)

static void mkdb(const char *path) {
  MDBX_env *env = NULL;
  CHK(mdbx_env_create(&env));
  mdbx_env_set_geometry(env, -1, -1, 16 << 20, -1, -1, -1);
  CHK(mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644));
  MDBX_txn *txn = NULL;
  CHK(mdbx_txn_begin(env, NULL, (MDBX_txn_flags_t)0, &txn));
  MDBX_dbi dbi;
  CHK(mdbx_dbi_open(txn, NULL, (MDBX_db_flags_t)0, &dbi));
  MDBX_val key = {.iov_base = (void *)"k", .iov_len = 1};
  MDBX_val value = {.iov_base = (void *)"v", .iov_len = 1};
  CHK(mdbx_put(txn, dbi, &key, &value, (MDBX_put_flags_t)0));
  CHK(mdbx_txn_commit(txn));
  mdbx_env_close(env);
}

TEST(issue_gh0059, failed_second_open_must_not_drop_first_handle_locks) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *dir = "issue_gh59";
  char db[512], lck[600];
  snprintf(db, sizeof(db), "%s/env.db", dir);
  snprintf(lck, sizeof(lck), "%s-lck", db);

  unlink(db);
  unlink(lck);
  rmdir(dir);
  if (mkdir(dir, 0755) && access(dir, F_OK)) {
    perror("mkdir");
    FAIL();
    return;
  }
  mkdb(db);

  /* the live handle */
  MDBX_env *first = NULL;
  CHK(mdbx_env_create(&first));
  CHK(mdbx_env_open(first, db, MDBX_NOSUBDIR, 0644));

  /* a second in-process open fails (BUSY on classic-fcntl, EAGAIN with OFD
   * locks) and must not disturb the first handle's locks */
  MDBX_env *second = NULL;
  CHK(mdbx_env_create(&second));
  int rc2 = mdbx_env_open(second, db, MDBX_NOSUBDIR, 0644);
  printf("second in-process open: %s (%d)\n", mdbx_strerror(rc2), rc2);
  EXPECT_NE(rc2, MDBX_SUCCESS);
  mdbx_env_close(second);

  /* a forked probe: exclusive access must still be denied */
  int pipefd[2];
  ASSERT_EQ(pipe(pipefd), 0);
  pid_t pid = fork();
  ASSERT_GE(pid, 0);
  if (pid == 0) {
    /* child: try to seize the environment exclusively */
    close(pipefd[0]);
    MDBX_env *probe = NULL;
    int rc = (mdbx_env_create(&probe) == MDBX_SUCCESS)
                 ? mdbx_env_open(probe, db, MDBX_NOSUBDIR | MDBX_EXCLUSIVE | MDBX_RDONLY, 0644)
                 : MDBX_PROBLEM;
    const ssize_t n = write(pipefd[1], &rc, sizeof(rc));
    (void)n;
    _exit(0);
  }
  close(pipefd[1]);
  int probe_rc = -1;
  ssize_t n = read(pipefd[0], &probe_rc, sizeof(probe_rc));
  ASSERT_EQ(n, (ssize_t)sizeof(probe_rc));
  close(pipefd[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  printf("probe after failed reopen: %s (%d)\n", mdbx_strerror(probe_rc), probe_rc);
  EXPECT_EQ(probe_rc, MDBX_BUSY) << "the first handle's file locks must survive a failed second open";

  mdbx_env_close(first);
  unlink(db);
  unlink(lck);
  rmdir(dir);
  printf("RESULT: locks intact\n");
}
