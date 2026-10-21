#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/103
// mdbx_txn_rollback() on a nested read-write transaction returned MDBX_SUCCESS
// but left the nested transaction finished: every later call on it and on its
// parent (which still had MDBX_TXN_HAS_CHILD) failed with MDBX_BAD_TXN, so the
// parent could no longer commit and its changes were lost; page lists leaked;
// debug builds aborted on the ro-nested assertion.

TEST(issue_gh0103, nested_write_rollback_keeps_running) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue103.mdbx";
  remove(testdb);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_max_db, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  MDBX_txn *w = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &w));
  MDBX_dbi main_dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(w, nullptr, MDBX_DB_DEFAULTS, &main_dbi));
  MDBX_val pk = {(void *)"parent-key", 9};
  MDBX_val pv = {(void *)"p", 1};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(w, main_dbi, &pk, &pv, MDBX_UPSERT));

  /* Nested write transaction: rollback must undo its changes and keep it
   * running (documented behavior), not finish it and poison the parent. */
  MDBX_txn *n = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, w, MDBX_TXN_READWRITE, &n));
  MDBX_val k1 = {(void *)"k1", 2}, v1 = {(void *)"v1", 2};
  MDBX_val k2 = {(void *)"k2", 2}, v2 = {(void *)"v2", 2};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(n, main_dbi, &k1, &v1, MDBX_UPSERT));

  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_rollback(n)) << "rollback of a nested write txn must succeed";

  /* The nested transaction must be usable again: the rolled-back key is gone,
   * the parent's data is visible, and writes work. */
  MDBX_val value;
  EXPECT_EQ(MDBX_NOTFOUND, mdbx_get(n, main_dbi, &k1, &value)) << "the rolled-back key must be gone";
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(n, main_dbi, &pk, &value)) << "the parent's data must be visible";
  EXPECT_EQ(MDBX_SUCCESS, mdbx_put(n, main_dbi, &k2, &v2, MDBX_UPSERT)) << "the nested txn must accept writes";
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(n)) << "the nested txn must commit";

  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(w, main_dbi, &k2, &value)) << "the parent must see the nested commit";
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(w)) << "the parent must commit";

  mdbx_env_close(env);
  remove(testdb);
}