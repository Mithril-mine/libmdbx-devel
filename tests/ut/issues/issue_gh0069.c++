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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/69
// mdbx_drop(txn, FREE_DBI, ...) was accepted and unconditionally ran
// tbl_purge() on the GC handle before the `dbi < CORE_DBS` check, destroying
// the garbage-collection tree — permanently leaking every page the GC tracked
// and corrupting page accounting — while returning MDBX_SUCCESS. The GC table
// must be refused up front.

#define FREE_DBI 0

static void remove_db(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }
}

static bool populate_gc(MDBX_env *env) {
  /* Insert in one write transaction, delete in a separate one, so that the
   * discarded pages are tracked by the GC tree (as in the issue's reproducer). */
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn) != MDBX_SUCCESS)
    return false;
  MDBX_dbi dbi = 0;
  if (mdbx_dbi_open(txn, "main", MDBX_CREATE, &dbi) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  for (int i = 0; i < 20000; ++i) {
    char kb[24], vb[24];
    const int kn = snprintf(kb, sizeof kb, "key%08d", i);
    const int vn = snprintf(vb, sizeof vb, "val%08d", i);
    MDBX_val key = {kb, (size_t)kn};
    MDBX_val val = {vb, (size_t)vn};
    if (mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      return false;
    }
  }
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }

  if (mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn) != MDBX_SUCCESS)
    return false;
  MDBX_cursor *mc = nullptr;
  if (mdbx_cursor_open(txn, dbi, &mc) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  MDBX_val key = {}, data = {};
  int rc;
  while ((rc = mdbx_cursor_get(mc, &key, &data, MDBX_NEXT)) == MDBX_SUCCESS) {
    if ((rc = mdbx_cursor_del(mc, MDBX_put_flags_t(0))) != MDBX_SUCCESS) {
      mdbx_cursor_close(mc);
      mdbx_txn_abort(txn);
      return false;
    }
  }
  mdbx_cursor_close(mc);
  if (rc != MDBX_NOTFOUND) {
    mdbx_txn_abort(txn);
    return false;
  }
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  return true;
}

TEST(issue_gh0069, free_dbi_drop_is_rejected_and_gc_survives) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue69.mdbx";
  remove_db(testdb);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));
  ASSERT_TRUE(populate_gc(env)) << "could not populate the GC tree";

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn));
  MDBX_stat st;
  std::memset(&st, 0, sizeof(st));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(txn, FREE_DBI, &st, sizeof(st)));
  const uint64_t entries_before = st.ms_entries;
  ASSERT_GT(entries_before, 0u) << "the GC tree should have tracked discarded pages";

  /* Clearing (or dropping) the GC table must be refused, not executed. */
  EXPECT_EQ(MDBX_EACCESS, mdbx_drop(txn, FREE_DBI, false)) << "mdbx_drop(FREE_DBI) must be rejected";

  std::memset(&st, 0, sizeof(st));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(txn, FREE_DBI, &st, sizeof(st)));
  EXPECT_EQ(entries_before, st.ms_entries) << "the GC tree must survive a rejected drop";

  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  remove_db(testdb);
}
