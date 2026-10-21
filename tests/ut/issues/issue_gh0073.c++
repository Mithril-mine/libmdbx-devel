#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/73
// cursor_check_multiple() computed an overflow guard by dividing a constant by
// the MDBX_MULTIPLE element size (data[0].iov_len) without checking it for
// zero. An element size of 0 with a huge element count crashed the process
// with a hardware divide-by-zero before any value-size validation ran. A
// DUPFIXED element size of zero is invalid regardless, so the put must return
// MDBX_BAD_VALSIZE instead of dividing by zero.

static void remove_db(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }
}

TEST(issue_gh0073, multiple_with_zero_element_size_is_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue73.mdbx";
  remove_db(testdb);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "dup", MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_CREATE, &dbi));

  MDBX_cursor *mc = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &mc));

  MDBX_val key = {const_cast<char *>("key1"), 4};
  char vbuf[8];
  std::memset(vbuf, 'x', sizeof(vbuf));
  MDBX_val data[2];
  data[0].iov_base = vbuf;
  data[0].iov_len = 0; /* element size: zero is invalid for DUPFIXED */
  data[1].iov_base = nullptr;
  data[1].iov_len = SIZE_MAX; /* huge element count to enter the overflow guard */

  EXPECT_EQ(MDBX_BAD_VALSIZE, mdbx_cursor_put(mc, &key, data, MDBX_MULTIPLE))
      << "a MDBX_MULTIPLE put with a zero element size must be rejected, not crash";

  mdbx_cursor_close(mc);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  remove_db(testdb);
}
