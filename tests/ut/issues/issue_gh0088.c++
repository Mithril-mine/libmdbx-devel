#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/88
// mdbx_gc_info() had two defects in its record loop:
//  1. in a write transaction, records already reclaimed by that transaction
//     were skipped with `continue`, which jumped past outer_next() so the
//     cursor stayed on the same record and the call never returned;
//  2. a non-zero visitor result was documented to stop the enumeration and be
//     returned immediately, but the break only left the inner span loop and
//     outer_next() then overwrote rc.
// The test exercises the visitor stop on a read transaction: a visitor that
// returns MDBX_RESULT_TRUE on its first call must make the call return that
// value immediately, not scan the remaining records.

struct stop_ctx {
  int calls;
};

static int stop_visitor(void *ctx, const MDBX_txn *txn, uint64_t span_txnid, size_t span_pgno, size_t span_length,
                        bool span_is_reclaimable) noexcept {
  (void)txn;
  (void)span_txnid;
  (void)span_pgno;
  (void)span_length;
  (void)span_is_reclaimable;
  auto *const c = static_cast<stop_ctx *>(ctx);
  ++c->calls;
  return MDBX_RESULT_TRUE;
}

TEST(issue_gh0088, gc_info_stops_on_visitor_return) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue88.mdbx";
  remove(testdb);
  struct stat st;
  (void)st;

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  /* Create a few GC records so the scan has something to iterate. */
  MDBX_dbi dbi;
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "kv", MDBX_CREATE, &dbi));
    for (unsigned i = 0; i < 50; ++i) {
      char key[16], val[128];
      snprintf(key, sizeof(key), "k%u", i);
      memset(val, 'a' + (i % 26), sizeof(val));
      MDBX_val k{key, strlen(key)}, v{val, sizeof(val)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }
  /* 3 more commit cycles, each retiring a chunk of pages into GC. */
  for (unsigned round = 0; round < 3; ++round) {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    for (unsigned i = 0; i < 15; ++i) {
      char key[16], val[128];
      snprintf(key, sizeof(key), "k2_%u_%u", round, i);
      memset(val, 'x' + round, sizeof(val));
      MDBX_val k{key, strlen(key)}, v{val, sizeof(val)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  stop_ctx ctx = {0};
  MDBX_gc_info_t info;
  memset(&info, 0, sizeof(info));
  MDBX_txn *rotxn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &rotxn));
  int rc = mdbx_gc_info(rotxn, &info, sizeof(info), stop_visitor, &ctx);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(rotxn));

  EXPECT_EQ(1, ctx.calls) << "the visitor stop must end the enumeration immediately";
  EXPECT_EQ(MDBX_RESULT_TRUE, rc) << "the visitor's non-zero result must be returned";

  mdbx_env_close(env);
  remove(testdb);
}