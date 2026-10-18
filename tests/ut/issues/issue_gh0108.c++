#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/108
// The MainDB table-record scan in tbl_stat_summary() used `goto next` to skip a
// table already accounted as an open handle, but the label was placed outside
// the per-node loop, so the first already-counted record on a leaf page aborted
// the scan of that page and the tables after it were never added: using even one
// named table in a transaction made mdbx_env_stat_ex() under-count everything
// after it on the same leaf page (regression since v0.14.2).

static void put_key(MDBX_txn *txn, MDBX_dbi dbi, const std::string &key) {
  MDBX_val key_val = {const_cast<char *>(key.data()), key.size()};
  MDBX_val val_val = {key_val.iov_base, key_val.iov_len};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key_val, &val_val, MDBX_UPSERT));
}

TEST(issue_gh0108, stat_ex_summary_not_shrinking_after_table_use) {
  const std::string testdb = "issue108.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644));

  /* Three tables on one MainDB leaf page: a=100, b=200, c=300 records. */
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    const struct {
      const char *name;
      int n;
    } tables[] = {{"a", 100}, {"b", 200}, {"c", 300}};
    for (const auto &t : tables) {
      MDBX_dbi dbi = 0;
      ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, t.name, MDBX_CREATE, &dbi));
      for (int i = 0; i < t.n; ++i)
        put_key(txn, dbi, "key" + std::to_string(i));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  MDBX_txn *rtxn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &rtxn));

  MDBX_stat st = {};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_stat_ex(env, rtxn, &st, sizeof(st)));
  const uint64_t before = st.ms_entries;

  /* Using one named table in this transaction must not change the totals. */
  MDBX_dbi a = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(rtxn, "a", MDBX_DB_DEFAULTS, &a));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(rtxn, a, &st, sizeof(st)));
  EXPECT_EQ((uint64_t)100, st.ms_entries) << "table `a` must hold its 100 records";

  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_stat_ex(env, rtxn, &st, sizeof(st)));
  const uint64_t after = st.ms_entries;
  EXPECT_EQ(before, after) << "using a table in the transaction must not hide the other tables of the same leaf page";
  EXPECT_GE(after, (uint64_t)(100 + 200 + 300)) << "the summary must account all table records";

  mdbx_txn_abort(rtxn);
  mdbx_env_close(env);
  mdbx::env::remove(testdb);
}