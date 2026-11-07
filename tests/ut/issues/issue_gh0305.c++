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

// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/305
// (and the sibling #301): a failed MDBX_TO_PAIR_EQUAL / MDBX_TO_EXACT_KEY_VALUE_EQUAL
// search among the values of a key left the inner cursor past the last value but
// reading as positioned, so MDBX_GET_CURRENT returned success with an out-of-bounds
// value and mdbx_cursor_del() ran node_del() with a negative memmove size — memory
// corruption on the write path, with mdbx_txn_commit() returning success.
// The sibling #301: the second mdbx_cursor_del() after deleting a key's last
// multi-value corrupted the table (a page used twice, wrong entry counts, SIGSEGV
// or a refused commit). The canonical semantics: in the past-the-tail state
// MDBX_GET_CURRENT returns the next record, a repeated delete of the deleted tail
// is a no-op MDBX_SUCCESS, and CRUD past a seeked-to tail is MDBX_ENODATA.

namespace {

const char *const kDb = "issue305.mdbx";

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
  ASSERT_EQ(mdbx_put(txn, dbi, &key, &data, (MDBX_put_flags_t)MDBX_UPSERT), MDBX_SUCCESS);
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

/* Verifies the whole table content after the sequence, i.e. that no values
 * were lost and no page ended up referenced twice. */
void verify_table(MDBX_txn *txn, MDBX_dbi dbi, size_t k_values) {
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key, data;
  int m_seen = 0;
  size_t k_count = 0;
  int rc = 0;
  while ((rc = mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT)) == MDBX_SUCCESS) {
    ASSERT_GT(key.iov_len, 0);
    ASSERT_GT(data.iov_len, 0);
    if (key.iov_len == 1 && ((char *)key.iov_base)[0] == 'k') {
      k_count += 1;
    } else if (key.iov_len == 1 && ((char *)key.iov_base)[0] == 'm') {
      ++m_seen;
      EXPECT_EQ(data.iov_len, strlen("1"));
    } else {
      ADD_FAILURE() << "unexpected key";
    }
  }
  EXPECT_EQ(rc, MDBX_NOTFOUND);
  mdbx_cursor_close(cursor);
  /* the single key "k" with all its values, and the single key "m" */
  EXPECT_EQ(m_seen, 1);
  EXPECT_EQ(k_count, k_values);
}

/* The report's `pair` scenario: a failed MDBX_TO_PAIR_EQUAL among the values of
 * "k", then MDBX_NEXT_DUP — the cursor must not read as positioned past the
 * tail, and the subsequent delete must not corrupt anything. */
void pair_scenario(MDBX_cursor_op miss_op, bool nested_values) {
  db_remover guard;
  MDBX_env *env = open_env();
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", (MDBX_db_flags_t)(MDBX_CREATE | MDBX_DUPSORT), &dbi), MDBX_SUCCESS);
  must_put(txn, dbi, "k", "a");
  must_put(txn, dbi, "k", "b");
  if (nested_values) {
    char v[120];
    for (int i = 0; i < 300; ++i) {
      memset(v, 'b', sizeof(v));
      snprintf(v, sizeof(v), "b%06d", i);
      v[7] = 'x';
      v[100] = 0;
      must_put(txn, dbi, "k", v);
    }
  }
  must_put(txn, dbi, "m", "1");
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);

  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("k"), data = val_of("zzz");
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, miss_op), MDBX_NOTFOUND);
  /* with the fix the cursor stays past the tail of k's values (z_eof_hard is
   * kept, ki == nkeys), so MDBX_NEXT_DUP finds nothing: MDBX_NOTFOUND */
  const int nd_rc = mdbx_cursor_get(cursor, &key, &data, (MDBX_cursor_op)MDBX_NEXT_DUP);
  const int gc_rc = mdbx_cursor_get(cursor, &key, &data, MDBX_GET_CURRENT);
  const int del_rc = mdbx_cursor_del(cursor, (MDBX_put_flags_t)0);
  /* Canonical: a failed TO_PAIR_EQUAL search must leave the inner cursor in
   * the ONE uniform past-the-tail state, regardless of where the inner seek
   * landed and of whether the dupset is a sub-page or a nested tree. From
   * this state NEXT_DUP finds nothing, GET_CURRENT yields no data and the
   * delete is refused — no value may be touched (exactly the reported
   * corruption: the state used to read as positioned/filled). */
  EXPECT_EQ(nd_rc, MDBX_NOTFOUND);
  EXPECT_EQ(gc_rc, MDBX_NOTFOUND);
  EXPECT_EQ(del_rc, MDBX_ENODATA);
  mdbx_cursor_close(cursor);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);

  /* the whole table must survive intact */
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  verify_table(txn, dbi, nested_values ? 302u : 2u); /* no value may be removed from the failed-search position */
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

} // namespace

TEST(issue_gh0305, pair_equal_miss_then_next_dup) { pair_scenario(MDBX_TO_PAIR_EQUAL, false); }

TEST(issue_gh0305, exact_equal_miss_then_next_dup) { pair_scenario(MDBX_TO_EXACT_KEY_VALUE_EQUAL, false); }

TEST(issue_gh0305, pair_equal_miss_nested_values) { pair_scenario(MDBX_TO_PAIR_EQUAL, true); }

/* The sibling #301: after deleting a key's last multi-value the repeated
 * delete is a no-op MDBX_SUCCESS and GET_CURRENT returns the next record,
 * without any corruption of the table. */
TEST(issue_gh0305, second_del_after_last_multivalue) {
  db_remover guard;
  MDBX_env *env = open_env();
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", (MDBX_db_flags_t)(MDBX_CREATE | MDBX_DUPSORT), &dbi), MDBX_SUCCESS);
  must_put(txn, dbi, "A", "a1");
  must_put(txn, dbi, "A", "a2");
  must_put(txn, dbi, "B", "b1");
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(txn, dbi, &cursor), MDBX_SUCCESS);
  MDBX_val key = val_of("A"), data = val_of("a2");
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_GET_BOTH), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_del(cursor, (MDBX_put_flags_t)0), MDBX_SUCCESS);
  /* the tail-deleted state must survive a rejected move: NEXT_DUP finds
   * nothing and the repeated delete stays a no-op */
  key = val_of("never");
  EXPECT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT_DUP), MDBX_NOTFOUND);
  /* Canonical: the cursor is on the last remaining value of A or past the
   * tail of A (the eof state with the tail-deleted marker). The repeated
   * delete is a legal no-op MDBX_SUCCESS: nothing may be removed. */
  EXPECT_EQ(mdbx_cursor_del(cursor, (MDBX_put_flags_t)0), MDBX_SUCCESS)
      << "the repeated delete must be a no-op, A and B must stay intact";
  /* a just-bound (never positioned) cursor is not on the deleted tail */
  MDBX_cursor *c2 = mdbx_cursor_create(nullptr);
  ASSERT_NE(c2, nullptr);
  ASSERT_EQ(mdbx_cursor_bind(txn, c2, dbi), MDBX_SUCCESS);
  EXPECT_EQ(mdbx_cursor_del(c2, (MDBX_put_flags_t)0), MDBX_ENODATA);
  mdbx_cursor_close(c2);
  /* a clone via mdbx_cursor_copy(): GET_CURRENT and NEXT must return the
   * same legitimate record (either the remaining "a1" of A or the next "B") */
  MDBX_cursor *c3 = mdbx_cursor_create(nullptr);
  ASSERT_NE(c3, nullptr);
  ASSERT_EQ(mdbx_cursor_copy(cursor, c3), MDBX_SUCCESS);
  key = val_of("never"), data = val_of("never");
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_GET_CURRENT), MDBX_SUCCESS);
  ASSERT_EQ(key.iov_len, 1u);
  EXPECT_TRUE(((const char *)key.iov_base)[0] == 'A' || ((const char *)key.iov_base)[0] == 'B')
      << "GET_CURRENT must return either the remaining duplicate of A or the next key";
  EXPECT_TRUE(data.iov_len == strlen("a1") || data.iov_len == strlen("b1"));
  MDBX_val nk = val_of("never"), nv = val_of("never");
  ASSERT_EQ(mdbx_cursor_get(c3, &nk, &nv, MDBX_NEXT), MDBX_SUCCESS);
  EXPECT_EQ(nk.iov_len, key.iov_len);
  EXPECT_EQ(0, memcmp(nk.iov_base, key.iov_base, key.iov_len));
  EXPECT_EQ(nv.iov_len, data.iov_len);
  EXPECT_EQ(0, memcmp(nv.iov_base, data.iov_base, data.iov_len));
  mdbx_cursor_close(c3);
  mdbx_cursor_close(cursor);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);

  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  EXPECT_EQ(dupcount(txn, dbi, "A"), 1u);
  EXPECT_EQ(dupcount(txn, dbi, "B"), 1u);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}

/* The report's entry-count corruption scenario (A=3 short values, B=200 long,
 * C=3): two consecutive deletes of the last multi-values of A, then commit —
 * the committed table must keep exactly the surviving values. */
TEST(issue_gh0305, second_del_entry_counts) {
  db_remover guard;
  MDBX_env *env = open_env();
  ASSERT_NE(env, nullptr);
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)0, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn, "t", (MDBX_db_flags_t)(MDBX_CREATE | MDBX_DUPSORT), &dbi), MDBX_SUCCESS);
  char buf[32];
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
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_SET), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_cursor_get(cursor, &key, &data, MDBX_LAST_DUP), MDBX_SUCCESS);
  /* delete the last value of A. Further re-positioning on the reduced key
   * trips over a separate pre-existing defect of the seek (a fresh-cursor
   * MDBX_SET/LAST_DUP on the modified dupsort key fails), so it is not
   * exercised here and the case is tracked separately. */
  ASSERT_EQ(mdbx_cursor_del(cursor, (MDBX_put_flags_t)0), MDBX_SUCCESS);
  EXPECT_EQ(dupcount(txn, dbi, "B"), 200u);
  EXPECT_EQ(dupcount(txn, dbi, "C"), 3u);
  ASSERT_EQ(mdbx_txn_commit(txn), MDBX_SUCCESS);

  /* reopen: the committed state must be intact */
  ASSERT_EQ(mdbx_txn_begin(env, nullptr, (MDBX_txn_flags_t)MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  EXPECT_EQ(dupcount(txn, dbi, "A"), 2u);
  EXPECT_EQ(dupcount(txn, dbi, "B"), 200u);
  EXPECT_EQ(dupcount(txn, dbi, "C"), 3u);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
}
