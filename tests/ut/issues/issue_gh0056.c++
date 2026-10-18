#include "mdbx.h++"
#include <cstdio>
#include <gtest/gtest.h>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/56
// mdbx_replace_ex() linked its on-stack cursor into txn->cursors[] at entry and
// unlinked it only at the common exit label; the aliased old/new buffers check
// returned MDBX_EINVAL directly, skipping the unlink. Every later operation on
// the same DBI then walked the cursor list into the dead stack frame: hang on
// Linux -O2 (self-referential next), stack-use-after-return/access-violation
// under (address)sanitizers, or silently corrupted state on other layouts.
TEST(issue_gh0056, aliased_old_new_buffers) {
  const std::string testdb = "issue56.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }

  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 64 << 20, -1, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "kv", MDBX_CREATE, &dbi));

  MDBX_val key = {const_cast<char *>("k"), 1};
  MDBX_val data = {const_cast<char *>("hello"), 5};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT));

  char buf[8] = "world";
  MDBX_val new_data = {buf, 5};
  MDBX_val old_data = {buf, 5}; // same iov_base as new_data
  EXPECT_EQ(MDBX_EINVAL, mdbx_replace_ex(txn, dbi, &key, &new_data, &old_data, MDBX_UPSERT, nullptr, nullptr));

  /* On the buggy code the on-stack cursor of mdbx_replace_ex() is still linked
   * into txn->cursors[dbi]; the following put then hangs (Linux -O2) or traps
   * with a stack-use-after-return/access-violation under (address)sanitizers. */
  MDBX_val key2 = {const_cast<char *>("k2"), 2};
  MDBX_val data2 = {const_cast<char *>("value2"), 6};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key2, &data2, MDBX_UPSERT));

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  mdbx_env_close(env);
}
