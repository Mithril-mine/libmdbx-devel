/// \copyright SPDX-License-Identifier: Apache-2.0
///
/// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/311
/// (items 1 and 2 of 3).
///
/// Item 1: a mdbx_cursor_delete_range() that fails half-way (a named table's
/// record inside the range, reached because the tree has large pages) used to
/// leave the transaction committable with a half-cut tree, so the caller who
/// saw the error reasonably assumed nothing changed, but the commit damaged
/// the database. Now the transaction is marked as failed and the commit
/// refuses it.
///

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdio>
#include <cstring>
#include <string>

#include "../test-env.h"

namespace {

struct env_ptr {
  MDBX_env *env = nullptr;
  ~env_ptr() {
    if (env)
      mdbx_env_close(env);
  }
  MDBX_env **operator&() { return &env; }
  MDBX_env *get() const { return env; }
};

struct txn_ptr {
  MDBX_txn *txn = nullptr;
  ~txn_ptr() {
    if (txn)
      mdbx_txn_abort(txn);
  }
  MDBX_txn **operator&() { return &txn; }
  MDBX_txn *get() const { return txn; }
  MDBX_txn *release() {
    MDBX_txn *t = txn;
    txn = nullptr;
    return t;
  }
};

struct cursor_ptr {
  MDBX_cursor *cursor = nullptr;
  ~cursor_ptr() {
    if (cursor)
      mdbx_cursor_close(cursor);
  }
  MDBX_cursor **operator&() { return &cursor; }
  MDBX_cursor *get() const { return cursor; }
};

void must_put(MDBX_txn *txn, MDBX_dbi dbi, const char *key, const void *value, size_t len) {
  MDBX_val k = {const_cast<void *>(static_cast<const void *>(key)), strlen(key)};
  MDBX_val v = {const_cast<void *>(value), len};
  ASSERT_EQ(mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
}

/* the MAIN_DBI, i.e. the table of the named tables */
constexpr MDBX_dbi main_table = 1;

/* deletes [from..to] with two positioned cursors, as the issue's repro does */
int delete_range(MDBX_txn *txn, MDBX_dbi dbi, MDBX_val *from, MDBX_cursor_op from_op, MDBX_val *to,
                 MDBX_cursor_op to_op, uint64_t *deleted) {
  cursor_ptr b, n;
  MDBX_val v;
  int rc = mdbx_cursor_open(txn, dbi, &b);
  if (rc != MDBX_SUCCESS)
    return rc;
  rc = mdbx_cursor_open(txn, dbi, &n);
  if (rc != MDBX_SUCCESS)
    return rc;
  rc = mdbx_cursor_get(b.get(), from, &v, from_op);
  if (rc != MDBX_SUCCESS)
    return rc;
  rc = mdbx_cursor_get(n.get(), to, &v, to_op);
  if (rc != MDBX_SUCCESS)
    return rc;
  rc = mdbx_cursor_delete_range(b.get(), n.get(), true, deleted);
  return rc;
}

/* Item 1 of 3: a failed range delete must make the transaction
 * uncommittable, since a partial cut cannot be undone. */
TEST(issue_gh0311, pp1_failed_range_delete_makes_txn_uncommittable) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0311f.mdbx";
  const env_files_guard guard({db.c_str()});
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_geometry(env.get(), -1, INT64_C(1) << 20, INT64_C(1) << 28, INT64_C(1) << 20, -1, 4096),
            MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  static char large[20000];
  memset(large, 'L', sizeof(large));
  const char small[] = "valuevaluevaluevaluevaluevaluevaluevaluevaluevalue";
  char key[32];
  txn_ptr txn;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  for (int i = 0; i < 2000; ++i) {
    snprintf(key, sizeof(key), "a%05d", i);
    must_put(txn.get(), main_table, key, small, sizeof(small) - 1);
    snprintf(key, sizeof(key), "z%05d", i);
    must_put(txn.get(), main_table, key, small, sizeof(small) - 1);
  }
  /* a large page makes the cutoff take the guarded path */
  must_put(txn.get(), main_table, "k1-large", large, sizeof(large));
  /* a named table's record inside the coming range */
  MDBX_dbi dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "k2-table", MDBX_CREATE, &dbi), MDBX_SUCCESS);
  must_put(txn.get(), dbi, "x", "y", 1);
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);

  /* the range delete must fail on the named table's record */
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  MDBX_val from = {const_cast<void *>(static_cast<const void *>("a00010")), 6};
  MDBX_val to = {const_cast<void *>(static_cast<const void *>("z01990")), 6};
  uint64_t deleted = 0;
  EXPECT_EQ(delete_range(txn.get(), main_table, &from, MDBX_SET_KEY, &to, MDBX_SET_KEY, &deleted), MDBX_INCOMPATIBLE);

  /* the commit must refuse the transaction with the partially cut tree */
  EXPECT_EQ(mdbx_txn_commit(txn.release()), MDBX_RESULT_TRUE);

  /* the database must be intact: nothing was deleted */
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  MDBX_dbi main_dbi = 0;
  ASSERT_EQ(mdbx_dbi_open(txn.get(), nullptr, MDBX_DB_DEFAULTS, &main_dbi), MDBX_SUCCESS);
  MDBX_val k = {const_cast<void *>(static_cast<const void *>("a00010")), 6}, v;
  ASSERT_EQ(mdbx_get(txn.get(), main_dbi, &k, &v), MDBX_SUCCESS);
  k.iov_base = const_cast<void *>(static_cast<const void *>("z01990"));
  ASSERT_EQ(mdbx_get(txn.get(), main_dbi, &k, &v), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), "k2-table", MDBX_DB_ACCEDE, &dbi), MDBX_SUCCESS);
  mdbx_txn_abort(txn.release());
}

} /* namespace */
