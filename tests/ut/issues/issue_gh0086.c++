/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/86
// mdbx_estimate_distance() returned a negated result when both cursors sat on
// different duplicates of one DUPSORT key: the outer level computed
// `last - first`, but the nested-duplicates level called cursor_diff() with the
// operands swapped (`first - last`), flipping the sign. The documented contract
// is `*distance_items = distance(first, last)` — positive for an ordered pair.

static void remove_db(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }
}

static MDBX_env *open_dupsort_env(const char *path, MDBX_dbi *dbi_out) {
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
  /* items: k1:a | k2:a k2:b k2:c k2:d k2:e | k3:a | k4:a */
  const char *const data[] = {"k1", "a", "k2", "a", "k2", "b", "k2", "c", "k2", "d", "k2", "e", "k3", "a", "k4", "a"};
  for (size_t i = 0; i < sizeof(data) / sizeof(data[0]); i += 2) {
    MDBX_val key = {const_cast<char *>(data[i]), (size_t)strlen(data[i])};
    MDBX_val val = {const_cast<char *>(data[i + 1]), (size_t)strlen(data[i + 1])};
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

/* Position a cursor on the exact (key, data) pair; returns false on failure. */
static bool at(MDBX_cursor *mc, const char *key_str, const char *data_str) {
  MDBX_val key = {const_cast<char *>(key_str), (size_t)strlen(key_str)};
  MDBX_val data = {const_cast<char *>(data_str), (size_t)strlen(data_str)};
  return mdbx_cursor_get(mc, &key, &data, MDBX_GET_BOTH) == MDBX_SUCCESS;
}

TEST(issue_gh0086, same_key_duplicates_distance_is_positive) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue86.mdbx";
  remove_db(testdb);
  MDBX_dbi dbi = 0;
  MDBX_env *env = open_dupsort_env(testdb, &dbi);
  ASSERT_NE(env, nullptr);

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_cursor *first = nullptr, *last = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &first));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &last));

  const char *const pairs[][2] = {{"a", "b"}, {"a", "e"}, {"c", "e"}, {"b", "e"}};
  for (const auto &pair : pairs) {
    ASSERT_TRUE(at(first, "k2", pair[0]));
    ASSERT_TRUE(at(last, "k2", pair[1]));
    ptrdiff_t distance = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_estimate_distance(first, last, &distance));
    EXPECT_GT(distance, 0) << "distance(first=" << pair[0] << ", last=" << pair[1] << ") must be positive";
  }

  mdbx_cursor_close(first);
  mdbx_cursor_close(last);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  remove_db(testdb);
}
