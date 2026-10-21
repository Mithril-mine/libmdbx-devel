#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/64
// A pure (empty) write transaction was committed through the full path even
// when there was nothing to persist: the fast-completion guard in
// txn_basal_commit() requires !head.is_steady, but in the default
// MDBX_SYNC_DURABLE mode the recent meta is steady, so the clause is false and
// an empty commit wrote a new meta page, advanced the txnid and (with the data
// file held open O_WRONLY|O_DSYNC) flushed the device — several milliseconds
// per no-op commit. The guard must skip the write when the head is already
// steady (nothing to persist) or the mode is lazy (MDBX_SAFE_NOSYNC /
// MDBX_NOMETASYNC, which carry no durability promise), and fall through to the
// full commit path only for the legitimate weak->steady upgrade in the durable
// mode.
// The tests measure the mi_recent_txnid before/after a batch of empty commits:
// a durable meta write advances it, the fast path does not.

static uint64_t recent_txnid(MDBX_env *env) {
  MDBX_envinfo ei;
  std::memset(&ei, 0, sizeof(ei));
  if (mdbx_env_info_ex(env, nullptr, &ei, sizeof(ei)) != MDBX_SUCCESS)
    return 0;
  return ei.mi_recent_txnid;
}

static void remove_db(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }
}

static bool put_key(MDBX_env *env, const char *key, const char *value, unsigned txn_flags) {
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  if (mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(txn_flags), &txn) != MDBX_SUCCESS)
    return false;
  if (mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  MDBX_val k = {const_cast<char *>(key), std::strlen(key)};
  MDBX_val v = {const_cast<char *>(value), std::strlen(value)};
  int rc = mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT);
  if (rc != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  rc = mdbx_txn_commit(txn);
  return rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE;
}

static bool commit_empty(MDBX_env *env, unsigned txn_flags) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(txn_flags), &txn) != MDBX_SUCCESS)
    return false;
  int rc = mdbx_txn_commit(txn);
  return rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE;
}

TEST(issue_gh0064, empty_durable_commits_do_not_advance_txnid) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue64-durable.mdbx";
  remove_db(testdb);
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 16 << 20, -1, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  /* a real durable commit establishes a steady head */
  ASSERT_TRUE(put_key(env, "seed", "value", /* MDBX_SYNC_DURABLE = */ 0));
  const uint64_t before = recent_txnid(env);
  ASSERT_GT(before, 0);

  for (int i = 0; i < 100; i++)
    ASSERT_TRUE(commit_empty(env, /* MDBX_SYNC_DURABLE = */ 0));
  /* empty durable commits must not write a meta page nor advance the txnid */
  EXPECT_EQ(before, recent_txnid(env));

  /* sanity: a non-empty durable commit still persists and advances the txnid */
  ASSERT_TRUE(put_key(env, "after", "value", /* MDBX_SYNC_DURABLE = */ 0));
  EXPECT_GT(recent_txnid(env), before);

  mdbx_env_close(env);
  remove_db(testdb);
}

TEST(issue_gh0064, empty_durable_commit_after_nometasync_keeps_meta_sync) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue64-nometa.mdbx";
  remove_db(testdb);
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 16 << 20, -1, -1, -1));
  /* open in MDBX_NOMETASYNC and write data: the meta page is written lazily,
   * so switching back to the durable mode leaves a pending meta flush */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR | MDBX_NOMETASYNC, 0644));
  ASSERT_TRUE(put_key(env, "lazy-1", "value", /* MDBX_SYNC_DURABLE = */ 0));
  const uint64_t before = recent_txnid(env);
  ASSERT_GT(before, 0);

  /* switch to the durable mode and commit an empty transaction: the pending
   * nometasync meta flush must still be performed (the empty commit falls
   * through to the full path), and the lazy-written data must become durable */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_flags(env, MDBX_NOMETASYNC, false));
  ASSERT_TRUE(commit_empty(env, /* MDBX_SYNC_DURABLE = */ 0));
  EXPECT_GT(recent_txnid(env), before);

  /* the data must survive a close + read-only reopen */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
  env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR | MDBX_RDONLY, 0644));
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
  MDBX_val k1 = {const_cast<char *>("lazy-1"), 6};
  MDBX_val data = {};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_get(txn, dbi, &k1, &data));
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  remove_db(testdb);
}

TEST(issue_gh0064, empty_commits_in_safe_nosync_env_do_not_advance_txnid) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue64-nosync.mdbx";
  remove_db(testdb);
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 16 << 20, -1, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644));

  /* a non-empty commit in the lazy mode writes a weak meta, so the head is weak */
  ASSERT_TRUE(put_key(env, "seed", "value", /* MDBX_SYNC_DURABLE = */ 0));
  const uint64_t before = recent_txnid(env);
  ASSERT_GT(before, 0);

  /* empty commits in the lazy mode must stay on the fast path: nothing to
   * persist and no durability promise, so no meta write and no txnid advance */
  for (int i = 0; i < 50; i++)
    ASSERT_TRUE(commit_empty(env, /* MDBX_SYNC_DURABLE = */ 0));
  EXPECT_EQ(before, recent_txnid(env));

  mdbx_env_close(env);
  remove_db(testdb);
}

TEST(issue_gh0064, empty_nosync_commits_do_not_advance_txnid) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue64-txn-nosync.mdbx";
  remove_db(testdb);
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 16 << 20, -1, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  ASSERT_TRUE(put_key(env, "seed", "value", /* MDBX_SYNC_DURABLE = */ 0));
  const uint64_t before = recent_txnid(env);
  ASSERT_GT(before, 0);

  /* empty transactions begun with MDBX_TXN_NOSYNC are pure too: no data to
   * write, no durability promised, so the meta write must be skipped */
  for (int i = 0; i < 100; i++)
    ASSERT_TRUE(commit_empty(env, MDBX_TXN_NOSYNC));
  EXPECT_EQ(before, recent_txnid(env));

  mdbx_env_close(env);
  remove_db(testdb);
}
