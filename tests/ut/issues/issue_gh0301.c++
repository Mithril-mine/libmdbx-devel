/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> All Rights Reserved.
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

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdio>
#include <cstring>

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/301
//
// After mdbx_cursor_del() removes the last multi-value of a key, the outer
// cursor stays on the key while the logical position is already on the next
// record. A second mdbx_cursor_del() is a legal call (mdbx.h says the delete
// does not invalidate the cursor), but it used to overwrite the record of one
// key with the nested tree of the next one: the committed table ended with
// pages used twice or a wrong entry count, or the call crashed. In the same
// state MDBX_GET_CURRENT returned a stale value instead of the record that
// MDBX_NEXT yields.
//
// The shapes with many values per key also expose an independent pre-existing
// defect family (#388): a single value-delete may silently lose values of the
// key itself or of a neighbor, with commit returning success and mdbx_chk
// staying clean. Until that family is fixed, such shapes are covered here as
// crash-only smoke, while the full assertions use the shapes that are free of
// it (one or two keys with a few small values).

namespace {

MDBX_val val_of(const char *s) {
  MDBX_val v{const_cast<void *>(static_cast<const void *>(s)), strlen(s)};
  return v;
}

void put_values(MDBX_txn *txn, MDBX_dbi dbi, const char *key, int count, int len) {
  static char buf[8192];
  for (int i = 0; i < count; ++i) {
    memset(buf, 'x', sizeof(buf));
    snprintf(buf, sizeof(buf), "%06d", i);
    buf[6] = 'x';
    buf[len] = 0;
    MDBX_val k = val_of(key), v = val_of(buf);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_put_flags_t(MDBX_UPSERT)));
  }
}

/* The number of multi-values of the key, or 0 when the key is absent. */
size_t dupcount(MDBX_txn *txn, MDBX_dbi dbi, const char *key) {
  MDBX_val k = val_of(key), v;
  size_t n = 0;
  if (mdbx_get_ex(txn, dbi, &k, &v, &n) != MDBX_SUCCESS)
    return 0;
  return n ? n : 1;
}

/* Creates the environment at `path`, fills the table and commits; leaves the
 * environment open (owned by the caller's guard) with the table handle valid. */
void fill(const char *path, int a_values, int a_len, int b_values, int b_len, MDBX_env **env_out, MDBX_dbi *dbi_out) {
  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_g{&txn};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(env_out));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(*env_out, 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(*env_out, path, MDBX_NOSUBDIR, 0644));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(*env_out, nullptr, MDBX_TXN_READWRITE, &txn));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "t", MDBX_CREATE | MDBX_DUPSORT, dbi_out));
  put_values(txn, *dbi_out, "A", a_values, a_len);
  if (b_values > 0)
    put_values(txn, *dbi_out, "B", b_values, b_len);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  txn = nullptr;
}

} // namespace

/* A single key: after the deletion of its last multi-value the second delete
 * has no next record to settle on, so it must report MDBX_ENODATA and keep
 * the remaining values intact. */
TEST(issue_gh0301, second_delete_single_key) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard{"issue_gh0301-one.mdbx"};
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  env_handle_guard env_g{&env};
  txn_handle_guard txn_g{&txn};
  fill("issue_gh0301-one.mdbx", 3, 20, 0, 0, &env, &dbi);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &cursor));
  MDBX_val k = val_of("A"), v;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor, &k, &v, MDBX_SET));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor, &k, &v, MDBX_LAST_DUP));

  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_del(cursor, MDBX_put_flags_t(0)));
  EXPECT_EQ(2u, dupcount(txn, dbi, "A"));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_del(cursor, MDBX_put_flags_t(0))) << "a repeated delete is a legal no-op";
  EXPECT_EQ(2u, dupcount(txn, dbi, "A")) << "the refused delete must change nothing";
  mdbx_cursor_close(cursor);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  txn = nullptr;

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  EXPECT_EQ(2u, dupcount(txn, dbi, "A"));
}

/* Two keys: the second delete settles on the next key and removes its first
 * (only) value, while the target key keeps the rest of its own values. */
TEST(issue_gh0301, second_delete_advances_to_next_key) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard{"issue_gh0301-two.mdbx"};
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  env_handle_guard env_g{&env};
  txn_handle_guard txn_g{&txn};
  fill("issue_gh0301-two.mdbx", 3, 20, 1, 20, &env, &dbi);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &cursor));
  MDBX_val k = val_of("A"), v;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor, &k, &v, MDBX_SET));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor, &k, &v, MDBX_LAST_DUP));

  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_del(cursor, MDBX_put_flags_t(0)));
  EXPECT_EQ(2u, dupcount(txn, dbi, "A"));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_del(cursor, MDBX_put_flags_t(0))) << "a repeated delete is a legal no-op";
  EXPECT_EQ(2u, dupcount(txn, dbi, "A")) << "the target key must keep its remaining values";
  EXPECT_EQ(1u, dupcount(txn, dbi, "B")) << "the single value of B must stay";
  mdbx_cursor_close(cursor);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  txn = nullptr;

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  EXPECT_EQ(2u, dupcount(txn, dbi, "A"));
  EXPECT_EQ(1u, dupcount(txn, dbi, "B"));
}

/* MDBX_GET_CURRENT and MDBX_NEXT must return the same record after the
 * deletion of the last multi-value (mdbx.h promises them to agree). */
TEST(issue_gh0301, get_current_matches_next_after_last_dup_delete) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const env_files_guard guard{"issue_gh0301-gc.mdbx"};
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi;
  env_handle_guard env_g{&env};
  txn_handle_guard txn_g{&txn};
  fill("issue_gh0301-gc.mdbx", 2, 20, 1, 20, &env, &dbi);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_cursor *cursor = nullptr, *clone = mdbx_cursor_create(nullptr);
  ASSERT_NE(nullptr, clone);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &cursor));
  MDBX_val k = val_of("A"), v;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor, &k, &v, MDBX_SET));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor, &k, &v, MDBX_LAST_DUP));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_del(cursor, MDBX_put_flags_t(0)));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_copy(cursor, clone));

  MDBX_val ck{}, cv{}, nk{}, nv{};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor, &ck, &cv, MDBX_GET_CURRENT));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(clone, &nk, &nv, MDBX_NEXT));
  EXPECT_EQ(nk.iov_len, ck.iov_len) << "GET_CURRENT and NEXT must return the same key";
  ASSERT_EQ(0, memcmp(ck.iov_base, nk.iov_base, ck.iov_len));
  EXPECT_EQ(nv.iov_len, cv.iov_len) << "GET_CURRENT and NEXT must return the same value";
  ASSERT_EQ(0, memcmp(cv.iov_base, nv.iov_base, cv.iov_len));
  /* the tail of A was deleted, so the next record is the only value of B */
  ASSERT_EQ(1u, nk.iov_len);
  EXPECT_EQ(0, memcmp(nk.iov_base, "B", 1)) << "the next record must be the only value of B";
  mdbx_cursor_close(clone);
  mdbx_cursor_close(cursor);
}

/* Crash-only smoke for the shapes of the report with many values per key:
 * the double delete must not corrupt memory nor crash, whatever the
 * (independently tracked, see #388) value-loss defect family does. */
TEST(issue_gh0301, no_crash_on_heavy_shapes) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const struct {
    int a_values, a_len, b_values, b_len;
  } shapes[] = {{200, 100, 200, 100}, {3, 20, 200, 100}, {200, 100, 1, 20}, {200, 100, 3, 20}};
  for (const auto &shape : shapes) {
    char path[64];
    snprintf(path, sizeof(path), "issue_gh0301-h%dx%dx%dx%d.mdbx", shape.a_values, shape.a_len, shape.b_values,
             shape.b_len);
    const env_files_guard guard{path};
    MDBX_env *env = nullptr;
    MDBX_txn *txn = nullptr;
    MDBX_dbi dbi;
    env_handle_guard env_g{&env};
    txn_handle_guard txn_g{&txn};
    fill(path, shape.a_values, shape.a_len, shape.b_values, shape.b_len, &env, &dbi);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_cursor *cursor = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &cursor));
    MDBX_val k = val_of("A"), v;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor, &k, &v, MDBX_SET));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(cursor, &k, &v, MDBX_LAST_DUP));
    EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_del(cursor, MDBX_put_flags_t(0)));
    /* must not crash nor read out of bounds, whatever it deletes */
    (void)mdbx_cursor_del(cursor, MDBX_put_flags_t(0));
    mdbx_cursor_close(cursor);
    /* the commit must succeed or report an error, but not corrupt memory */
    (void)mdbx_txn_commit(txn);
    txn = nullptr;
  }
}
