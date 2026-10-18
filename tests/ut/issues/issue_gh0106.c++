#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/106
// mdbx_dbi_rename()/mdbx_dbi_rename2() stores txn->dbs[dbi] (the transaction's
// cached table record) under the new name without refreshing the handle when it
// is DBI_STALE: a handle first used in the write transaction may hold the record
// left by an earlier aborted write transaction (or by another process), so the
// commit persists a tree whose root points to pages that were never committed —
// the database is silently corrupted. The test drives the documented sequence
// (create + commit, modify + abort, rename + commit) and then verifies the
// renamed table still matches what was really committed.

static void put_key(MDBX_txn *txn, MDBX_dbi dbi, const std::string &key) {
  MDBX_val key_val = {const_cast<char *>(key.data()), key.size()};
  const std::string value = "v" + key;
  MDBX_val val_val = {const_cast<char *>(value.data()), value.size()};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key_val, &val_val, MDBX_UPSERT));
}

TEST(issue_gh0106, rename_after_aborted_write) {
  const std::string testdb = "issue106.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644));

  /* txn1: create "t" and commit a small committed baseline. */
  MDBX_dbi t = 0;
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "t", MDBX_CREATE, &t));
    for (int i = 0; i < 10; ++i)
      put_key(txn, t, "key" + std::to_string(i));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  /* txn2: modify the table heavily, then abort: the transaction's cached tree
   * record keeps the aborted root and counters. */
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    for (int i = 0; i < 5000; ++i)
      put_key(txn, t, "more" + std::to_string(i));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(txn));
  }

  /* txn3: rename "t" -> "u" (the first use of the handle in this transaction,
   * so it is DBI_STALE): the stored record must be the committed one. */
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_rename(txn, t, "u"));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  /* Read "u" back: it must contain exactly the committed baseline. */
  {
    MDBX_txn *rtxn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &rtxn));
    MDBX_dbi u = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(rtxn, "u", MDBX_DB_DEFAULTS, &u));
    MDBX_stat st = {};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(rtxn, u, &st, sizeof(st)));
    EXPECT_EQ((uint64_t)10, st.ms_entries) << "the renamed table must contain the committed records, not the stale "
                                              "record of the aborted transaction";

    MDBX_cursor *cur = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(rtxn, u, &cur));
    size_t walked = 0;
    MDBX_val key = {}, value = {};
    int rc = mdbx_cursor_get(cur, &key, &value, MDBX_FIRST);
    while (rc == MDBX_SUCCESS) {
      ++walked;
      rc = mdbx_cursor_get(cur, &key, &value, MDBX_NEXT);
    }
    mdbx_cursor_close(cur);
    if (rc != MDBX_NOTFOUND)
      FAIL() << "the cursor walk over the renamed table ended with the error code " << rc;
    EXPECT_EQ((size_t)10, walked);
    mdbx_txn_abort(rtxn);
  }

  mdbx_env_close(env);
  mdbx::env::remove(testdb);
}