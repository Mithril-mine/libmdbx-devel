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
/// \author Andrea Lanfranchi (https://github.com/AndreaLanfranchi)

#include "mdbx.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/304
// A put into an existing multi-value key through one cursor (or mdbx_put())
// left every other cursor positioned on that key with a stale copy of the
// key's nested tree record; a later mdbx_cursor_del() through such a cursor
// wrote the stale record back or used its stale item count, so committed
// values were lost and pages ended up referenced twice, while
// mdbx_txn_commit() returned success.

namespace {

const char *const kDb = "issue304.mdbx";

void remove_db() {
  std::remove(kDb);
  std::remove((std::string(kDb) + MDBX_LOCK_SUFFIX).c_str());
}

struct db_remover {
  db_remover() { remove_db(); }
  ~db_remover() { remove_db(); }
};

MDBX_val val_of(const char *s) {
  MDBX_val v = {(void *)s, strlen(s)};
  return v;
}

void must_put(MDBX_txn *txn, MDBX_dbi dbi, const char *k, const char *v) {
  MDBX_val key = val_of(k), data = val_of(v);
  ASSERT_EQ(mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT), MDBX_SUCCESS);
}

void fill_dups(MDBX_txn *txn, MDBX_dbi dbi, const char *k, int n, int len) {
  char buf[8192];
  for (int i = 0; i < n; ++i) {
    memset(buf, 'x', sizeof(buf));
    snprintf(buf, sizeof(buf), "%06d", i);
    buf[6] = 'x';
    buf[len] = 0;
    must_put(txn, dbi, k, buf);
  }
}

size_t dupcount(MDBX_txn *txn, MDBX_dbi dbi, const char *k) {
  MDBX_val key = val_of(k), data;
  size_t n = 0;
  EXPECT_EQ(mdbx_get_ex(txn, dbi, &key, &data, &n), MDBX_SUCCESS);
  return n;
}

MDBX_env *open_env() {
  MDBX_env *env = nullptr;
  EXPECT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  EXPECT_NE(env, nullptr);
  if (env) {
    EXPECT_EQ(mdbx_env_set_maxdbs(env, 2), MDBX_SUCCESS);
    EXPECT_EQ(mdbx_env_set_geometry(env, -1, 1 << 20, 256 << 20, 1 << 20, -1, 4096), MDBX_SUCCESS);
    if (mdbx_env_open(env, kDb, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
      mdbx_env_close(env);
      return nullptr;
    }
  }
  return env;
}

void fill_and_commit(MDBX_env *env, MDBX_dbi &dbi) {
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_CREATE | MDBX_DUPSORT, &dbi), MDBX_SUCCESS);
  fill_dups(txn, dbi, "K", 2000, 100);
  fill_dups(txn, dbi, "L", 3, 20);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
}

/* Nested tree: c1 puts into key K, then c2, positioned on K before the put,
   deletes. When `nested` is set, c2 is opened in the parent transaction and
   the put and the delete run in a nested one (txn_shadow_cursors() moves it
   into the nested transaction, where it is a bystander like any other). */
void tree_case(bool nested) {
  db_remover guard;
  MDBX_env *env = open_env();
  ASSERT_NE(env, nullptr);
  MDBX_dbi dbi = 0;
  fill_and_commit(env, dbi);

  MDBX_txn *t = nullptr, *w = nullptr;
  MDBX_cursor *c1 = nullptr, *c2 = nullptr;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &t), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_open(t, dbi, &c2), MDBX_SUCCESS);
  MDBX_val key = val_of("K"), data;
  ASSERT_EQ(mdbx_cursor_get(c2, &key, &data, MDBX_SET), MDBX_SUCCESS);
  w = t;
  if (nested) {
    ASSERT_EQ(mdbx_txn_begin(env, t, (MDBX_txn_flags_t)0, &w), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_cursor_open(w, dbi, &c1), MDBX_SUCCESS);
  key = val_of("K");
  data = val_of("new-value");
  ASSERT_EQ(mdbx_cursor_put(c1, &key, &data, (MDBX_put_flags_t)MDBX_UPSERT), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_del(c2, (MDBX_put_flags_t)0), MDBX_SUCCESS);
  mdbx_cursor_close(c1);
  if (nested) {
    ASSERT_EQ(mdbx_txn_commit(w), MDBX_SUCCESS);
  }
  mdbx_cursor_close(c2);
  ASSERT_EQ(mdbx_txn_commit(t), MDBX_SUCCESS);

  /* the committed table must keep all 2000 values including the put one */
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)MDBX_TXN_RDONLY, &t), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_open(t, dbi, &c1), MDBX_SUCCESS);
  EXPECT_EQ(dupcount(t, dbi, "K"), 2000u);
  key = val_of("K");
  data = val_of("new-value");
  EXPECT_EQ(mdbx_cursor_get(c1, &key, &data, MDBX_GET_BOTH), MDBX_SUCCESS);
  mdbx_cursor_close(c1);
  mdbx_txn_abort(t);
  mdbx_env_close(env);
}

// The tests reproduce the defect on the unmodified devel; enabled here
// since the fix lands together with them.
TEST(issue_gh0304, tree_case_same_transaction) { tree_case(false); }

TEST(issue_gh0304, tree_case_nested_writer) { tree_case(true); }

/* Sub-page: a cursor positioned on A, then a plain mdbx_put() adds a value to
   A, then the cursor deletes twice — the stale item count must not drop the
   just-put value. */
TEST(issue_gh0304, subpage_case) {
  db_remover guard;
  MDBX_env *env = open_env();
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", MDBX_CREATE | MDBX_DUPSORT, &dbi), MDBX_SUCCESS);
  must_put(txn, dbi, "A", "v1");
  must_put(txn, dbi, "A", "v2");
  must_put(txn, dbi, "B", "b1");
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("A"), data;
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_SET), MDBX_SUCCESS);
  must_put(txn, dbi, "A", "v3");
  ASSERT_EQ(mdbx_cursor_del(cursor, (MDBX_put_flags_t)0), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_del(cursor, (MDBX_put_flags_t)0), MDBX_SUCCESS);
  EXPECT_EQ(dupcount(txn, dbi, "A"), 1u);
  key = val_of("A");
  data = val_of("v3");
  EXPECT_EQ(mdbx_get_ex(txn, dbi, &key, &data, nullptr), MDBX_SUCCESS);
  EXPECT_EQ(data.iov_len, strlen("v3"));
  mdbx_cursor_close(cursor);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);
  mdbx_env_close(env);
}

} // namespace
