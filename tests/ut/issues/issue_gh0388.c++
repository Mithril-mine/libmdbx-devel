/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but
/// not limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "mdbx.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../test-env.h"

// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/388
// The issue's scenario: the table holds the key "A" with three multi-values
// next to a big nested-tree key "B" (200 values) and "C"; one cursor deletes
// A's multi-values one by one, re-positioning via MDBX_SET + MDBX_LAST_DUP
// before every delete. The last round's MDBX_SET — with exactly one value
// left in A — used to return MDBX_NOTFOUND (and a fresh cursor's
// MDBX_SET/MDBX_LAST_DUP ended with MDBX_ENODATA), although the key exists
// and is readable via mdbx_get_ex(). The root cause: mdbx_cursor_del() of a
// key's last-but-one multi-value zeroed the caller's MDBX_val key/data
// arguments... which are not arguments of del() at all — actually it
// clobbered the user's MDBX_val through the positioned-output contract of
// the preceding reads; the effect is a silently emptied `key` for the next
// MDBX_SET, i.e. a search for the empty key.

namespace {

const char *const kDb = "issue388.mdbx";

void remove_db() {
  std::remove(kDb);
  std::remove((std::string(kDb) + MDBX_LOCK_SUFFIX).c_str());
}

struct db_remover {
  db_remover() { remove_db(); }
  ~db_remover() { remove_db(); }
};

MDBX_val val_of(const char *s) {
  MDBX_val v{(void *)s, strlen(s)};
  return v;
}

void must_put(MDBX_txn *txn, MDBX_dbi dbi, const char *k, const char *v) {
  MDBX_val key = val_of(k), data = val_of(v);
  ASSERT_EQ(mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT), MDBX_SUCCESS);
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

size_t dupcount(MDBX_txn *txn, MDBX_dbi dbi, const char *k) {
  MDBX_val key = val_of(k), data;
  size_t n = 0;
  const int rc = mdbx_get_ex(txn, dbi, &key, &data, &n);
  EXPECT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_NOTFOUND);
  return (rc == MDBX_SUCCESS) ? n : 0;
}

TEST(issue_gh0388, reduced_single_value) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  db_remover guard;
  MDBX_env *env = open_env();
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", (MDBX_db_flags_t)(MDBX_CREATE | MDBX_DUPSORT), &dbi), MDBX_SUCCESS);
  char buf[16];
  for (int i = 0; i < 3; ++i) {
    snprintf(buf, sizeof(buf), "%06d", i);
    must_put(txn, dbi, "A", buf);
  }
  for (int i = 0; i < 200; ++i) {
    snprintf(buf, sizeof(buf), "%06d", i);
    must_put(txn, dbi, "B", buf);
  }
  for (int i = 0; i < 3; ++i) {
    snprintf(buf, sizeof(buf), "%06d", i);
    must_put(txn, dbi, "C", buf);
  }
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);

  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("A"), data;
  for (int round = 1; round <= 3; ++round) {
    /* correct usage: the key must be re-filled before every positioning,
     * since the pointers returned by mdbx_cursor_get() point into the
     * database pages and a delete rewrites them */
    key = val_of("A");
    ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_SET), MDBX_SUCCESS)
        << "round " << round << ": MDBX_SET must succeed while the key exists";
    ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_LAST_DUP), MDBX_SUCCESS) << "round " << round;
    ASSERT_EQ(mdbx_cursor_del(cursor, (MDBX_put_flags_t)0), MDBX_SUCCESS) << "round " << round;
  }
  mdbx_cursor_close(cursor);
  EXPECT_EQ(dupcount(txn, dbi, "B"), 200u);
  EXPECT_EQ(dupcount(txn, dbi, "C"), 3u);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);

  /* reopen: the committed state must be intact */
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  EXPECT_EQ(dupcount(txn, dbi, "A"), 0u);
  EXPECT_EQ(dupcount(txn, dbi, "B"), 200u);
  EXPECT_EQ(dupcount(txn, dbi, "C"), 3u);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* The positive half of the issue: after reducing a key to a single value,
 * a fresh cursor must be able to MDBX_SET on the (still existing) key and
 * read its remaining value, incl. MDBX_LAST_DUP. */
TEST(issue_gh0388, reduced_to_single_value_set_works) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  db_remover guard;
  MDBX_env *env = open_env();
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", (MDBX_db_flags_t)(MDBX_CREATE | MDBX_DUPSORT), &dbi), MDBX_SUCCESS);
  must_put(txn, dbi, "A", "a000000");
  must_put(txn, dbi, "A", "a000001");
  must_put(txn, dbi, "B", "b1");
  must_put(txn, dbi, "C", "c1");
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);

  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("A"), data;
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_SET), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_LAST_DUP), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_del(cursor, (MDBX_put_flags_t)0), MDBX_SUCCESS);
  EXPECT_EQ(dupcount(txn, dbi, "A"), 1u);
  mdbx_cursor_close(cursor);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);

  /* a fresh cursor must see and read the reduced key */
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  EXPECT_EQ(dupcount(txn, dbi, "A"), 1u);
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  key = val_of("A");
  data = val_of("never");
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_SET), MDBX_SUCCESS) << "MDBX_SET on the existing reduced key";
  EXPECT_EQ(data.iov_len, strlen("a000000"));
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_LAST_DUP), MDBX_SUCCESS);
  EXPECT_EQ(data.iov_len, strlen("a000000"));
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

} // namespace
