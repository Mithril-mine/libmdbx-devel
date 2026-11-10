/// \copyright SPDX-License-Identifier: Apache-2.0
///
/// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/321
///
/// When a mid-commit flush of the IO ring fails (a write error such as EIO or ENOSPC), iov_complete()
/// has already released the shadow pages queued before the failure, but txn_write() returned at once
/// and left those pages in the dirty list. The same mdbx_txn_commit() then released them a second
/// time: large pages were passed to free() twice (glibc aborts), single pages were pushed twice onto
/// the shadow-page reserve list. The commit must report the write error, and the transaction must
/// remain abortable without heap corruption.
///
/// The pwrite family is wrapped at link time (--wrap), so every write fails with EIO while the test
/// flag is set. Requires the static libmdbx link, skipped for other configurations.

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "../test-env.h"

namespace {

struct env_ptr {
  MDBX_env *env = nullptr;
  ~env_ptr() {
    if (env)
      mdbx_env_close(env);
  }
  MDBX_env **operator&() { return &env; }
  MDBX_env *get() const { return env; }
  void close() {
    if (env) {
      mdbx_env_close(env);
      env = nullptr;
    }
  }
};

struct txn_ptr {
  MDBX_txn *txn = nullptr;
  ~txn_ptr() {
    if (txn)
      mdbx_txn_abort(txn);
  }
  MDBX_txn **operator&() { return &txn; }
  MDBX_txn *get() const { return txn; }
  MDBX_txn *release() {
    MDBX_txn *t = txn;
    txn = nullptr;
    return t;
  }
};

} /* namespace */

extern "C" {
int mdbx_test_io_fail = 0;

ssize_t __real_pwrite(int fd, const void *buf, size_t count, off_t offset);
ssize_t __wrap_pwrite(int fd, const void *buf, size_t count, off_t offset) {
  if (mdbx_test_io_fail) {
    errno = EIO;
    return -1;
  }
  return __real_pwrite(fd, buf, count, offset);
}

ssize_t __real_pwrite64(int fd, const void *buf, size_t count, off_t offset);
ssize_t __wrap_pwrite64(int fd, const void *buf, size_t count, off_t offset) {
  if (mdbx_test_io_fail) {
    errno = EIO;
    return -1;
  }
  return __real_pwrite64(fd, buf, count, offset);
}

struct iovec;
ssize_t __real_pwritev(int fd, const struct iovec *iov, int count, off_t offset);
ssize_t __wrap_pwritev(int fd, const struct iovec *iov, int count, off_t offset) {
  if (mdbx_test_io_fail) {
    errno = EIO;
    return -1;
  }
  return __real_pwritev(fd, iov, count, offset);
}

ssize_t __real_pwritev64(int fd, const struct iovec *iov, int count, off_t offset);
ssize_t __wrap_pwritev64(int fd, const struct iovec *iov, int count, off_t offset) {
  if (mdbx_test_io_fail) {
    errno = EIO;
    return -1;
  }
  return __real_pwritev64(fd, iov, count, offset);
}
} /* extern "C" */

TEST(issue_gh0321, commit_reports_write_error_without_heap_corruption) {
#if !defined(MDBX_TEST_PWRITE_WRAP)
  GTEST_SKIP() << "SKIPPED: the GNU ld --wrap feature is unavailable on this platform/linker";
#else
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0321.mdbx";
  const env_files_guard guard({db.c_str()});
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_geometry(env.get(), -1, -1, INT64_C(1) << 30, INT64_C(1) << 24, -1, 256), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_option(env.get(), MDBX_opt_txn_dp_limit, UINT64_C(1) << 20), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  txn_ptr txn;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn.get(), nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
  char key[16], val[300];
  memset(val, 'v', sizeof(val));
  MDBX_val k, v = {val, sizeof(val)};
  for (unsigned i = 0; i < 70000; ++i) {
    k.iov_len = (size_t)snprintf(key, sizeof(key), "%08u", i);
    k.iov_base = key;
    ASSERT_EQ(mdbx_put(txn.get(), dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
  }

  mdbx_test_io_fail = 1;
  MDBX_txn *const committing = txn.release();
  const int rc = mdbx_txn_commit(committing);
  mdbx_test_io_fail = 0;
  EXPECT_NE(rc, MDBX_SUCCESS) << "the write error must be reported";
  EXPECT_EQ(rc, MDBX_EIO) << "the injected EIO must be reported";
  /* the failed commit has ended the transaction itself */
  EXPECT_NE(mdbx_txn_abort(committing), MDBX_SUCCESS) << "the failed commit has ended the transaction";

  /* the database must survive and stay openable/readable */
  env.close();
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR | MDBX_RDONLY, 0644), MDBX_SUCCESS);
  txn_ptr rd;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &rd), MDBX_SUCCESS);
  MDBX_dbi dbi_rd = 0;
  ASSERT_EQ(mdbx_dbi_open(rd.get(), nullptr, MDBX_DB_DEFAULTS, &dbi_rd), MDBX_SUCCESS);
  MDBX_val chk_k = {const_cast<void *>(static_cast<const void *>("00000000")), 8}, chk_v;
  ASSERT_EQ(mdbx_get(rd.get(), dbi_rd, &chk_k, &chk_v), MDBX_NOTFOUND) << "nothing was committed";
#endif /* MDBX_TEST_PWRITE_WRAP */
}
