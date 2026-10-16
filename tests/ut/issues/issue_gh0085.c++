#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/85
// mdbx_cursor_scroll() and mdbx_cursor_distance() passed the unsigned
// `deepness` straight into the `int level` parameter of the per-level helpers.
// A value above INT_MAX (e.g. UINT_MAX, the natural way to say "unlimited")
// became a negative level, the cursor stack top was set to -1 and pg[-1]/ki[-1]
// were read/written out of bounds. mdbx_cursor_distribute() already clamps; the
// siblings must clamp the same way.

static void remove_db(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }
}

static MDBX_env *open_env(const char *path, MDBX_dbi *dbi_out) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return nullptr;
  if (mdbx_env_set_maxdbs(env, 8) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  if (mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }

  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  MDBX_dbi dbi = 0;
  if (mdbx_dbi_open(txn, "multi", MDBX_DUPSORT | MDBX_CREATE, &dbi) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    mdbx_env_close(env);
    return nullptr;
  }
  for (int i = 0; i < 16; ++i) {
    char kb[16], vb[16];
    const int kn = snprintf(kb, sizeof kb, "key%02d", i);
    const int vn = snprintf(vb, sizeof vb, "val%02d", i);
    MDBX_val key = {kb, (size_t)kn};
    MDBX_val val = {vb, (size_t)vn};
    if (mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      mdbx_env_close(env);
      return nullptr;
    }
  }
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    mdbx_env_close(env);
    return nullptr;
  }
  *dbi_out = dbi;
  return env;
}

TEST(issue_gh0085, scroll_with_unlimited_deepness_is_safe) {
  const char *const testdb = "issue85.mdbx";
  remove_db(testdb);
  MDBX_dbi dbi = 0;
  MDBX_env *env = open_env(testdb, &dbi);
  ASSERT_NE(env, nullptr);

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_cursor *mc = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &mc));

  MDBX_val key = {}, data = {};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(mc, &key, &data, MDBX_FIRST));
  /* deepness above INT_MAX used to underflow to level -1 and index pg[-1] */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_scroll(mc, 1, UINT_MAX))
      << "scrolling forward with deepness=UINT_MAX must not corrupt the cursor";

  /* and the reversed direction */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_scroll(mc, -1, UINT_MAX))
      << "scrolling backward with deepness=UINT_MAX must not corrupt the cursor";

  mdbx_cursor_close(mc);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  remove_db(testdb);
}

TEST(issue_gh0085, distance_with_unlimited_deepness_is_safe) {
  const char *const testdb = "issue85b.mdbx";
  remove_db(testdb);
  MDBX_dbi dbi = 0;
  MDBX_env *env = open_env(testdb, &dbi);
  ASSERT_NE(env, nullptr);

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_cursor *begin = nullptr, *end = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &begin));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &end));

  MDBX_val key = {}, data = {};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(begin, &key, &data, MDBX_FIRST));
  MDBX_val last_key = {const_cast<char *>("key15"), 5};
  MDBX_val last_data = {};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(end, &last_key, &last_data, MDBX_SET));

  intptr_t distance = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_distance(begin, end, &distance, UINT_MAX))
      << "cursor_distance() with deepness=UINT_MAX must not corrupt the cursor";
  EXPECT_GT(distance, 0);

  mdbx_cursor_close(begin);
  mdbx_cursor_close(end);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  remove_db(testdb);
}
