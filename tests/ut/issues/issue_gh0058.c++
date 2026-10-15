/* issue #58 — mdbx_env_copy(..., MDBX_CP_OVERWRITE) / mdbx_copy -f must not
 * delete a pre-existing destination database on POSIX when it cannot lock it.
 * The destination is not created by this call; it belongs to another live
 * environment. The failure happens before any write, so the file must
 * survive. */
#include "mdbx.h"
#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHK(expr)                                                                                                      \
  do {                                                                                                                 \
    int err_ = (expr);                                                                                                 \
    if (err_ != MDBX_SUCCESS) {                                                                                        \
      fprintf(stderr, "%s:%d: %s -> %s (%d)\n", __FILE__, __LINE__, #expr, mdbx_strerror(err_), err_);                 \
      exit(2);                                                                                                         \
    }                                                                                                                  \
  } while (0)

static void mkdb(const char *path, const char *tag) {
  MDBX_env *env = NULL;
  CHK(mdbx_env_create(&env));
  mdbx_env_set_geometry(env, -1, -1, 16 << 20, -1, -1, -1);
  CHK(mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644));
  MDBX_txn *txn = NULL;
  CHK(mdbx_txn_begin(env, NULL, (MDBX_txn_flags_t)0, &txn));
  MDBX_dbi dbi;
  CHK(mdbx_dbi_open(txn, NULL, (MDBX_db_flags_t)0, &dbi));
  MDBX_val key = {.iov_base = (void *)"k", .iov_len = 1};
  MDBX_val value = {.iov_base = (void *)tag, .iov_len = strlen(tag)};
  CHK(mdbx_put(txn, dbi, &key, &value, (MDBX_put_flags_t)0));
  CHK(mdbx_txn_commit(txn));
  mdbx_env_close(env);
}

static int exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

TEST(issue_gh0058, copy_overwrite_must_not_delete_live_destination) {
  const char *dir = "issue_gh58";
  char src_db[512], dst_db[512], lck[600];
  snprintf(src_db, sizeof(src_db), "%s/src.db", dir);
  snprintf(dst_db, sizeof(dst_db), "%s/dst.db", dir);
  snprintf(lck, sizeof(lck), "%s-lck", dst_db);

  /* fresh state */
  unlink(src_db);
  unlink(dst_db);
  unlink(lck);
  rmdir(dir);
  if (mkdir(dir, 0755) && access(dir, F_OK)) {
    perror("mkdir");
    FAIL();
    return;
  }
  mkdb(src_db, "source");
  mkdb(dst_db, "PRECIOUS-DESTINATION");

  /* another live holder of the destination */
  MDBX_env *live = NULL;
  CHK(mdbx_env_create(&live));
  CHK(mdbx_env_open(live, dst_db, MDBX_NOSUBDIR, 0644));

  /* the copy source, opened read-only */
  MDBX_env *src = NULL;
  CHK(mdbx_env_create(&src));
  CHK(mdbx_env_open(src, src_db, MDBX_NOSUBDIR | MDBX_RDONLY, 0644));

  int rc = mdbx_env_copy(src, dst_db, MDBX_CP_OVERWRITE);
  printf("mdbx_env_copy(MDBX_CP_OVERWRITE): %s (%d)\n", mdbx_strerror(rc), rc);
  /* The invariant this test guards is that the destination is never deleted,
   * whatever the outcome of the copy. On classic-fcntl builds an in-process
   * lock does not conflict (fcntl locks are per-process), so the copy may
   * legitimately succeed and overwrite the file; with OFD locks it fails with
   * a lock conflict, and then the live holder must still read its data. */
  EXPECT_TRUE(exists(dst_db)) << "the live destination must survive a failed copy";

  if (rc != MDBX_SUCCESS) {
    /* the live holder must still be able to read its value */
    MDBX_txn *txn = NULL;
    CHK(mdbx_txn_begin(live, NULL, MDBX_TXN_RDONLY, &txn));
    MDBX_dbi dbi;
    CHK(mdbx_dbi_open(txn, NULL, (MDBX_db_flags_t)0, &dbi));
    MDBX_val key = {.iov_base = (void *)"k", .iov_len = 1};
    MDBX_val value = {.iov_base = NULL, .iov_len = 0};
    rc = mdbx_get(txn, dbi, &key, &value);
    ASSERT_EQ(rc, MDBX_SUCCESS);
    EXPECT_EQ(value.iov_len, strlen("PRECIOUS-DESTINATION"));
    EXPECT_EQ(memcmp(value.iov_base, "PRECIOUS-DESTINATION", value.iov_len), 0);
    mdbx_txn_abort(txn);
  }

  mdbx_env_close(src);
  mdbx_env_close(live);

  unlink(src_db);
  unlink(dst_db);
  unlink(lck);
  rmdir(dir);
  printf("RESULT: destination survived, holder still readable\n");
}
