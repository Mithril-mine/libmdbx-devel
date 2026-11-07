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

#include "mdbx.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>

#include "../test-env.h"

#include <string>
#include <vector>

/* Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/301
 *
 * On a MDBX_DUPSORT table, after mdbx_cursor_del() removes the last
 * multi-value of a key, the outer cursor stays on the key and the inner
 * cursor is left in z_eof_hard. mdbx.h says the delete does not invalidate
 * the cursor, so a second mdbx_cursor_del() is a legal call — but it used to
 * overwrite the record of one key with the nested tree of the next one:
 * the committed table ended up with pages used twice or a wrong entry count,
 * or the call crashed.
 *
 * In the same state MDBX_GET_CURRENT returned the deleted-from key's
 * previous value instead of the next record, and MDBX_NEXT skipped ahead.
 *
 * Scenarios from the issue (A-values A-len B-values B-len):
 *   200 100 200 100 — page used twice;
 *     3  20 200 100 — wrong entry count;
 *   200 100   1  20 — SIGSEGV on the second delete;
 *   200 100   3  20 — commit refused.
 */

namespace {

constexpr MDBX_env_flags_t kEnvFlags = MDBX_NOSUBDIR;

struct fixture_t {
  MDBX_env *env = nullptr;
  MDBX_txn *txn = nullptr;
  MDBX_dbi dbi = 0;
  std::string path;

  ~fixture_t() {
    if (env)
      mdbx_env_close(env);
  }
};

void must(MDBX_env *env, int rc, int line, const char *what) {
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "issue_gh0301: line %d: %s -> %s (%d)\n", line, what, mdbx_strerror(rc), rc);
    if (env)
      mdbx_env_close(env);
    GTEST_FAIL() << what << " -> " << mdbx_strerror(rc) << " (" << rc << ") at line " << line;
  }
}

#define MUST(env, expr) must(env, (expr), __LINE__, #expr)

MDBX_val val_of(const char *s) {
  MDBX_val v{const_cast<void *>(static_cast<const void *>(s)), strlen(s)};
  return v;
}

/* Fills `n` duplicate values of the key, each of `len` bytes:
 * "000000x...." where the first 6 chars are the loop counter. */
void fill_dups(MDBX_env *env, MDBX_txn *txn, MDBX_dbi dbi, const char *key, int n, int len) {
  static char buf[8192];
  for (int i = 0; i < n; ++i) {
    memset(buf, 'x', sizeof(buf));
    snprintf(buf, sizeof(buf), "%06d", i);
    buf[6] = 'x';
    buf[len] = 0;
    MDBX_val a = val_of(key), b = val_of(buf);
    MUST(env, mdbx_put(txn, dbi, &a, &b, MDBX_put_flags_t(MDBX_UPSERT)));
  }
}

size_t dupcount(MDBX_txn *txn, MDBX_dbi dbi, const char *key) {
  MDBX_val a = val_of(key), b;
  size_t n = 0;
  return mdbx_get_ex(txn, dbi, &a, &b, &n) ? 0 : n;
}

/* Checks that a second mdbx_cursor_del() after the deletion of the last
 * multi-value of a key does not corrupt the table and that the commit
 * succeeds. */
void check_second_delete(const char *path, int a_values, int a_len, int b_values, int b_len) {
  fixture_t fx;
  fx.path = path;

  MUST(fx.env, mdbx_env_create(&fx.env));
  MUST(fx.env, mdbx_env_set_maxdbs(fx.env, 2));
  MUST(fx.env, mdbx_env_set_geometry(fx.env, -1, 1 << 20, 256 << 20, 1 << 20, -1, 4096));
  MUST(fx.env, mdbx_env_open(fx.env, path, kEnvFlags, 0644));

  MUST(fx.env, mdbx_txn_begin(fx.env, nullptr, MDBX_TXN_READWRITE, &fx.txn));
  MUST(fx.env, mdbx_dbi_open(fx.txn, "t", MDBX_CREATE | MDBX_DUPSORT, &fx.dbi));
  fill_dups(fx.env, fx.txn, fx.dbi, "A", a_values, a_len);
  fill_dups(fx.env, fx.txn, fx.dbi, "B", b_values, b_len);
  fill_dups(fx.env, fx.txn, fx.dbi, "C", 3, 20);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(fx.txn));
  fx.txn = nullptr;

  MUST(fx.env, mdbx_txn_begin(fx.env, nullptr, MDBX_TXN_READWRITE, &fx.txn));
  MDBX_cursor *c = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(fx.txn, fx.dbi, &c));

  MDBX_val k = val_of("A"), v;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(c, &k, &v, MDBX_SET));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(c, &k, &v, MDBX_LAST_DUP));

  /* delete the last multi-value of the key "A" */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_del(c, MDBX_put_flags_t(0)));
  /* mdbx.h: the delete does not invalidate the cursor — the second delete
   * is a legal call, it must not corrupt the table nor crash. */
  EXPECT_EQ(MDBX_SUCCESS, mdbx_cursor_del(c, MDBX_put_flags_t(0)));

  const size_t count_a = dupcount(fx.txn, fx.dbi, "A");
  const size_t count_b = dupcount(fx.txn, fx.dbi, "B");
  EXPECT_EQ(a_values - 1u, count_a) << "A must keep all its duplicates except the deleted one";
  EXPECT_EQ(b_values, count_b) << "B must keep all its duplicates";

  mdbx_cursor_close(c);
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(fx.txn));
  fx.txn = nullptr;

  /* Verify the committed table: re-open and count both keys again, then
   * probe every remaining duplicate of "A" for readability. */
  MUST(fx.env, mdbx_txn_begin(fx.env, nullptr, MDBX_TXN_RDONLY, &fx.txn));
  EXPECT_EQ(a_values - 1u, dupcount(fx.txn, fx.dbi, "A"));
  EXPECT_EQ(b_values, dupcount(fx.txn, fx.dbi, "B"));
  MDBX_cursor *rcursor = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(fx.txn, fx.dbi, &rcursor));
  k = val_of("A");
  /* a fresh cursor is not filled, so position it on the key first */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(rcursor, &k, &v, MDBX_SET));
  int rc = mdbx_cursor_get(rcursor, &k, &v, MDBX_FIRST_DUP);
  while (rc == MDBX_SUCCESS) {
    ASSERT_EQ(a_len, (int)v.iov_len) << "a leftover duplicate must keep its size";
    rc = mdbx_cursor_get(rcursor, &k, &v, MDBX_NEXT_DUP);
  }
  EXPECT_EQ(MDBX_NOTFOUND, rc);
  mdbx_cursor_close(rcursor);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(fx.txn));
  fx.txn = nullptr;
}

/* Checks MDBX_GET_CURRENT and MDBX_NEXT after the deletion of the last
 * multi-value of a key: both must return the same (next) record. */
void check_getcur(const char *path) {
  fixture_t fx;
  fx.path = path;

  MUST(fx.env, mdbx_env_create(&fx.env));
  MUST(fx.env, mdbx_env_set_maxdbs(fx.env, 2));
  MUST(fx.env, mdbx_env_open(fx.env, path, kEnvFlags, 0644));

  MUST(fx.env, mdbx_txn_begin(fx.env, nullptr, MDBX_TXN_READWRITE, &fx.txn));
  MUST(fx.env, mdbx_dbi_open(fx.txn, "t", MDBX_CREATE | MDBX_DUPSORT, &fx.dbi));
  MDBX_val k = val_of("A"), v = val_of("a1");
  MUST(fx.env, mdbx_put(fx.txn, fx.dbi, &k, &v, MDBX_put_flags_t(MDBX_UPSERT)));
  v = val_of("a2");
  MUST(fx.env, mdbx_put(fx.txn, fx.dbi, &k, &v, MDBX_put_flags_t(MDBX_UPSERT)));
  k = val_of("B");
  v = val_of("b1");
  MUST(fx.env, mdbx_put(fx.txn, fx.dbi, &k, &v, MDBX_put_flags_t(MDBX_UPSERT)));
  MDBX_cursor *c = nullptr, *c2 = mdbx_cursor_create(nullptr);
  ASSERT_NE(nullptr, c2);
  MUST(fx.env, mdbx_cursor_open(fx.txn, fx.dbi, &c));
  k = val_of("A");
  v = val_of("a2");
  MUST(fx.env, mdbx_cursor_get(c, &k, &v, MDBX_GET_BOTH));
  MUST(fx.env, mdbx_cursor_del(c, MDBX_put_flags_t(0)));
  MUST(fx.env, mdbx_cursor_copy(c, c2));

  /* After the delete of the last duplicate the cursor stays on the key "A"
   * with the inner cursor in z_eof_hard. Both MDBX_GET_CURRENT (on the
   * original cursor) and MDBX_NEXT (on its clone) must return the same
   * (next) record: either the remaining duplicate of "A" or the key "B". */
  MDBX_val ck{}, cv{};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(c, &ck, &cv, MDBX_GET_CURRENT));
  MDBX_val nk{}, nv{};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_get(c2, &nk, &nv, MDBX_NEXT));
  EXPECT_EQ(nk.iov_len, ck.iov_len) << "GET_CURRENT and NEXT must return the same key";
  EXPECT_EQ(0, memcmp(ck.iov_base, nk.iov_base, ck.iov_len));
  EXPECT_EQ(nv.iov_len, cv.iov_len) << "GET_CURRENT and NEXT must return the same value";
  EXPECT_EQ(0, memcmp(cv.iov_base, nv.iov_base, cv.iov_len));

  mdbx_cursor_close(c2);
  mdbx_cursor_close(c);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(fx.txn));
  fx.txn = nullptr;
}

} // namespace

TEST(issue_gh0301, second_delete_after_last_dup) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *path = "issue_gh0301-sd.mdbx";
  std::remove(path);
  check_second_delete(path, 200, 100, 200, 100);
  std::remove(path);
  /* the shape that used to produce a wrong entry count */
  check_second_delete(path, 3, 20, 200, 100);
  std::remove(path);
  /* the shape that used to SIGSEGV on the second delete */
  check_second_delete(path, 200, 100, 1, 20);
  std::remove(path);
  /* the shape that used to refuse the commit */
  check_second_delete(path, 200, 100, 3, 20);
  std::remove(path);
}

TEST(issue_gh0301, get_current_and_next_after_last_dup_delete) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *path = "issue_gh0301-gc.mdbx";
  std::remove(path);
  check_getcur(path);
  std::remove(path);
}
