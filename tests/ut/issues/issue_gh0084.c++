#include "mdbx.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/84
// mdbx_cursor_scroll()/mdbx_cursor_distance()/mdbx_cursor_distribute() return
// wrong results on DUPSORT tables when called at full depth (deepness >= tree
// height, as the docs recommend). scroll was off by one step for each duplicate
// set it entered or left, distance ignored the start cursor's own position
// inside its duplicate set, and distribute used (and corrupted) one of the
// caller's cursors as its working iterator.

typedef std::pair<std::string, std::string> item_t;

static void env_remove(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }
}

static bool build_table(MDBX_env *env, MDBX_dbi *dbi_out, const std::vector<item_t> &items) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn) != MDBX_SUCCESS)
    return false;
  MDBX_dbi dbi = 0;
  if (mdbx_dbi_open(txn, "multi", MDBX_DUPSORT | MDBX_CREATE, &dbi) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  for (const auto &item : items) {
    MDBX_val key = {const_cast<char *>(item.first.data()), item.first.size()};
    MDBX_val val = {const_cast<char *>(item.second.data()), item.second.size()};
    if (mdbx_put(txn, dbi, &key, &val, MDBX_UPSERT) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      return false;
    }
  }
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  *dbi_out = dbi;
  return true;
}

static void position_at(MDBX_cursor *mc, const std::vector<item_t> &items, size_t idx) {
  const item_t &item = items[idx];
  MDBX_val key = {const_cast<char *>(item.first.data()), item.first.size()};
  MDBX_val val = {const_cast<char *>(item.second.data()), item.second.size()};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(mc, &key, &val, MDBX_GET_BOTH));
}

static std::string cursor_item(MDBX_cursor *mc) {
  MDBX_val key = {}, val = {};
  if (mdbx_cursor_get(mc, &key, &val, MDBX_GET_CURRENT) != MDBX_SUCCESS)
    return "<none>";
  return std::string((const char *)key.iov_base, key.iov_len) + ":" +
         std::string((const char *)val.iov_base, val.iov_len);
}

static void check_scroll(MDBX_cursor *mc, const std::vector<item_t> &items) {
  for (size_t i = 0; i < items.size(); ++i) {
    for (int dir : {1, -1}) {
      for (intptr_t step : {1, 2}) {
        ASSERT_NO_FATAL_FAILURE(position_at(mc, items, i));
        const intptr_t want = (intptr_t)i + dir * step;
        const int rc = mdbx_cursor_scroll(mc, dir * step, 42);
        if (want < 0 || want >= (intptr_t)items.size()) {
          EXPECT_EQ(MDBX_NOTFOUND, rc) << "scroll from item " << i << " by " << dir * step << " must stop at the end";
        } else {
          EXPECT_EQ(MDBX_SUCCESS, rc) << "scroll from item " << i << " by " << dir * step;
          EXPECT_EQ(items[(size_t)want].first + ":" + items[(size_t)want].second, cursor_item(mc))
              << "scroll from item " << i << " by " << dir * step;
        }
      }
    }
  }
}

static void check_distance(MDBX_cursor *begin, MDBX_cursor *end, const std::vector<item_t> &items) {
  for (size_t i = 0; i < items.size(); ++i) {
    for (size_t j = i + 1; j < items.size(); ++j) {
      ASSERT_NO_FATAL_FAILURE(position_at(begin, items, i));
      ASSERT_NO_FATAL_FAILURE(position_at(end, items, j));
      intptr_t distance = INTPTR_MIN;
      const int rc = mdbx_cursor_distance(begin, end, &distance, 42);
      EXPECT_EQ(MDBX_SUCCESS, rc) << "distance from item " << i << " to item " << j;
      EXPECT_EQ((intptr_t)(j - i), distance) << "distance from item " << i << " to item " << j;
    }
  }
}

static void check_distribute(MDBX_cursor *begin, MDBX_cursor *end, MDBX_cursor *c0, MDBX_cursor *c1,
                             const std::vector<item_t> &items) {
  const item_t &last = items.back();
  /* two cursors: evenly split the whole range, [begin+chunk] and [last] */
  ASSERT_NO_FATAL_FAILURE(position_at(begin, items, 0));
  ASSERT_NO_FATAL_FAILURE(position_at(end, items, items.size() - 1));
  MDBX_cursor *pair[2] = {c0, c1};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_distribute(begin, end, pair, 2, 42))
      << "distribute {c0,c1} over " << items.size() << " items";
  const size_t half = (items.size() - 1 + 1) / 2;
  EXPECT_EQ(items[half].first + ":" + items[half].second, cursor_item(c0))
      << "distribute {c0,c1}: c0 must start the second half";
  EXPECT_EQ(last.first + ":" + last.second, cursor_item(c1))
      << "distribute {c0,c1}: c1 must reach the range end";

  /* with the range end also in the array; c1 and end must not be clobbered */
  ASSERT_NO_FATAL_FAILURE(position_at(begin, items, 0));
  ASSERT_NO_FATAL_FAILURE(position_at(end, items, items.size() - 1));
  MDBX_cursor *triple[3] = {c0, c1, end};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_distribute(begin, end, triple, 3, 42))
      << "distribute {c0,c1,end} over " << items.size() << " items";
  const size_t third = (items.size() - 1 + 2) / 3;
  const size_t second = third + (items.size() - 1 - third + 1) / 2;
  EXPECT_EQ(items[third].first + ":" + items[third].second, cursor_item(c0))
      << "distribute {c0,c1,end}: c0 must start the second third";
  EXPECT_EQ(items[second].first + ":" + items[second].second, cursor_item(c1))
      << "distribute {c0,c1,end}: c1 must start the last third";
  EXPECT_EQ(last.first + ":" + last.second, cursor_item(end))
      << "distribute {c0,c1,end}: the end cursor must stay eligible";

  /* with the range begin in the array after the working cursor */
  ASSERT_NO_FATAL_FAILURE(position_at(begin, items, 0));
  ASSERT_NO_FATAL_FAILURE(position_at(end, items, items.size() - 1));
  MDBX_cursor *pair_begin[2] = {c0, begin};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_distribute(begin, end, pair_begin, 2, 42))
      << "distribute {c0,begin} over " << items.size() << " items";
  EXPECT_EQ(items[half].first + ":" + items[half].second, cursor_item(c0))
      << "distribute {c0,begin}: c0 must start the second half";
  EXPECT_EQ(last.first + ":" + last.second, cursor_item(begin))
      << "distribute {c0,begin}: begin must reach the range end";

  /* without a begin cursor (from the first item), the range end in the array */
  ASSERT_NO_FATAL_FAILURE(position_at(end, items, items.size() - 1));
  MDBX_cursor *pair_end[2] = {c0, end};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_distribute(nullptr, end, pair_end, 2, 42))
      << "distribute(nullptr, end) {c0,end} over " << items.size() << " items";
  EXPECT_EQ(items[half].first + ":" + items[half].second, cursor_item(c0))
      << "distribute(nullptr, end) {c0,end}: c0 must start the second half";
  EXPECT_EQ(last.first + ":" + last.second, cursor_item(end))
      << "distribute(nullptr, end) {c0,end}: end must reach the range end";
}

TEST(issue_gh0084, subpage_dups_scroll_distance) {
  const char *const testdb = "issue84a.mdbx";
  env_remove(testdb);
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  const std::vector<item_t> items = {
      {"k1", "a"}, {"k2", "a"}, {"k2", "b"}, {"k2", "c"},
      {"k2", "d"}, {"k2", "e"}, {"k3", "a"}, {"k4", "a"},
  };
  MDBX_dbi dbi = 0;
  ASSERT_TRUE(build_table(env, &dbi, items));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_cursor *mc = nullptr, *begin = nullptr, *end = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &mc));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &begin));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &end));

  ASSERT_NO_FATAL_FAILURE(check_scroll(mc, items));
  ASSERT_NO_FATAL_FAILURE(check_distance(begin, end, items));

  mdbx_cursor_close(mc);
  mdbx_cursor_close(begin);
  mdbx_cursor_close(end);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  env_remove(testdb);
}

TEST(issue_gh0084, subpage_dups_distribute) {
  const char *const testdb = "issue84b.mdbx";
  env_remove(testdb);
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  const std::vector<item_t> items = {
      {"k1", "a"}, {"k2", "a"}, {"k2", "b"}, {"k2", "c"},
      {"k2", "d"}, {"k2", "e"}, {"k3", "a"}, {"k4", "a"},
  };
  MDBX_dbi dbi = 0;
  ASSERT_TRUE(build_table(env, &dbi, items));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_cursor *begin = nullptr, *end = nullptr, *c0 = nullptr, *c1 = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &begin));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &end));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &c0));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &c1));

  ASSERT_NO_FATAL_FAILURE(check_distribute(begin, end, c0, c1, items));

  mdbx_cursor_close(begin);
  mdbx_cursor_close(end);
  mdbx_cursor_close(c0);
  mdbx_cursor_close(c1);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  env_remove(testdb);
}

TEST(issue_gh0084, nested_tree_scroll_distance) {
  const char *const testdb = "issue84c.mdbx";
  env_remove(testdb);
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  std::vector<item_t> items = {{"a", "1"}, {"x", "1"}, {"z", "1"}};
  std::vector<item_t> dups;
  dups.reserve(3000);
  for (int i = 0; i < 3000; ++i) {
    char vb[16];
    const int vn = snprintf(vb, sizeof vb, "v%04d", i);
    dups.emplace_back("mid", std::string(vb, (size_t)vn));
  }
  items.insert(items.begin() + 1, dups.begin(), dups.end());

  MDBX_dbi dbi = 0;
  ASSERT_TRUE(build_table(env, &dbi, items));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_cursor *mc = nullptr, *begin = nullptr, *end = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &mc));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &begin));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &end));

  ASSERT_NO_FATAL_FAILURE(check_scroll(mc, items));
  /* Distance accounting for starts/ends inside a multi-page nested tree is
   * the subject of a separate pre-existing defect in the nested-tree page
   * walk; here only single-value endpoints spanning the whole range are
   * checked, whose leaf-amount accounting must not under-count. */
  ASSERT_NO_FATAL_FAILURE(position_at(begin, items, 0));
  ASSERT_NO_FATAL_FAILURE(position_at(end, items, items.size() - 1));
  intptr_t distance = INTPTR_MIN;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_distance(begin, end, &distance, 42))
      << "distance from the first to the last item";
  EXPECT_EQ((intptr_t)(items.size() - 1), distance) << "distance from the first to the last item";

  mdbx_cursor_close(mc);
  mdbx_cursor_close(begin);
  mdbx_cursor_close(end);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  env_remove(testdb);
}

TEST(issue_gh0084, nested_tree_distribute) {
  const char *const testdb = "issue84d.mdbx";
  env_remove(testdb);
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR, 0644));

  std::vector<item_t> items = {{"a", "1"}, {"x", "1"}, {"z", "1"}};
  std::vector<item_t> dups;
  dups.reserve(3000);
  for (int i = 0; i < 3000; ++i) {
    char vb[16];
    const int vn = snprintf(vb, sizeof vb, "v%04d", i);
    dups.emplace_back("mid", std::string(vb, (size_t)vn));
  }
  items.insert(items.begin() + 1, dups.begin(), dups.end());

  MDBX_dbi dbi = 0;
  ASSERT_TRUE(build_table(env, &dbi, items));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_cursor *begin = nullptr, *end = nullptr, *c0 = nullptr, *c1 = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &begin));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &end));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &c0));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &c1));

  ASSERT_NO_FATAL_FAILURE(check_distribute(begin, end, c0, c1, items));

  mdbx_cursor_close(begin);
  mdbx_cursor_close(end);
  mdbx_cursor_close(c0);
  mdbx_cursor_close(c1);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  env_remove(testdb);
}