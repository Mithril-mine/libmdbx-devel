/// \copyright SPDX-License-Identifier: Apache-2.0
///
/// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/326
///
/// When a nested write transaction's commit fails inside nested_join() (an
/// allocation failure), the parent's cursors must be restored from their
/// backups instead of being merged with the failed nested transaction's
/// state. Without the fix the merged cursors reference the pages released by
/// the nested transaction's rollback: garbage reads, MDBX_CORRUPTED or a
/// segfault. The parent transaction itself must remain usable and commitable
/// afterwards, committing exactly its own data.
///
/// Requires a probes-enabled build: the failure is injected via the probe-bus
/// fault site of nested_join(), which fires before any mutation and thus is
/// deterministic and platform-independent.

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdio>
#include <cstring>
#include <string>

#include "../probe-ctl.h"
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

void must_put(MDBX_txn *txn, MDBX_dbi dbi, const char *key, const char *value) {
  MDBX_val k = {const_cast<void *>(static_cast<const void *>(key)), strlen(key)};
  MDBX_val v = {const_cast<void *>(static_cast<const void *>(value)), strlen(value)};
  ASSERT_EQ(mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
}

TEST(issue_gh0326, failed_nested_commit_restores_parent_cursors) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0326.mdbx";
  const env_files_guard guard({db.c_str()});
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_maxdbs(env.get(), 4), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  /* the parent's uncommitted fill and its cursor */
  MDBX_dbi dbi = 0;
  txn_ptr parent;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &parent), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(parent.get(), "t", MDBX_CREATE, &dbi), MDBX_SUCCESS);
  char key[32], val[64];
  for (int i = 0; i < 2000; ++i) {
    snprintf(key, sizeof(key), "k%08d", i);
    snprintf(val, sizeof(val), "parent-%08d", i);
    must_put(parent.get(), dbi, key, val);
  }
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(mdbx_cursor_open(parent.get(), dbi, &cursor), MDBX_SUCCESS);
  MDBX_val ck = {const_cast<void *>(static_cast<const void *>("k00000000")), 9}, cv;
  ASSERT_EQ(mdbx_cursor_get(cursor, &ck, &cv, MDBX_SET), MDBX_SUCCESS);

  /* the nested transaction: a lot of new keys before the parent's ones, so
   * the tree restructures and the nested txn allocates new pages */
  txn_ptr child;
  ASSERT_EQ(mdbx_txn_begin(env.get(), parent.get(), MDBX_TXN_READWRITE, &child), MDBX_SUCCESS);
  for (int i = 0; i < 80000; ++i) {
    snprintf(key, sizeof(key), "c%08d", i);
    snprintf(val, sizeof(val), "child-%08d-child-child-child-child-child-child-child", i);
    must_put(child.get(), dbi, key, val);
  }
  /* make the parent's cursor take the nested transaction's page stack:
   * walk backward into the pages allocated by the nested transaction */
  for (int i = 0; i < 50; ++i) {
    MDBX_val k, v;
    if (mdbx_cursor_get(cursor, &k, &v, MDBX_PREV) != MDBX_SUCCESS)
      break;
  }

  /* fail the nested commit at the nested_join() entry, where nothing has
   * been mutated yet */
  char reply[256];
  ASSERT_EQ(0, mdbx_probe_ctl("fault nested_join_entry_enomem 12", reply, sizeof reply));
  const int commit_rc = mdbx_txn_commit(child.release());
  ASSERT_EQ(0, mdbx_probe_ctl("fault nested_join_entry_enomem none", reply, sizeof reply));
  EXPECT_NE(MDBX_SUCCESS, commit_rc) << "the injected allocation failure must fail the nested commit";
  /* a failed nested commit releases the child txn handle itself, do not abort it */

  /* the parent's plain access must serve the parent's data */
  MDBX_val gk = {const_cast<void *>(static_cast<const void *>("k00000001")), 9}, gv;
  ASSERT_EQ(mdbx_get(parent.get(), dbi, &gk, &gv), MDBX_SUCCESS);
  EXPECT_EQ(0, memcmp(gv.iov_base, "parent-00000001", 15));

  /* the cursor must be restored to the pre-child position, not merged with
   * the failed nested transaction's state (the core assertion of the issue) */
  MDBX_val rk, rv;
  ASSERT_EQ(mdbx_cursor_get(cursor, &rk, &rv, MDBX_GET_CURRENT), MDBX_SUCCESS);
  EXPECT_EQ(rk.iov_len, 9u);
  EXPECT_EQ(0, memcmp(rk.iov_base, "k00000000", 9));
  EXPECT_EQ(0, memcmp(rv.iov_base, "parent-00000000", 15));

  /* the parent must remain commitable, with exactly its own data */
  mdbx_cursor_close(cursor);
  ASSERT_EQ(mdbx_txn_commit(parent.release()), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &parent), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(parent.get(), "t", MDBX_DB_ACCEDE, &dbi), MDBX_SUCCESS);
  MDBX_val bad = {const_cast<void *>(static_cast<const void *>("c00000000")), 9}, bv;
  EXPECT_EQ(mdbx_get(parent.get(), dbi, &bad, &bv), MDBX_NOTFOUND);
  MDBX_val last = {const_cast<void *>(static_cast<const void *>("k00001999")), 9}, lv;
  ASSERT_EQ(mdbx_get(parent.get(), dbi, &last, &lv), MDBX_SUCCESS);
  EXPECT_EQ(0, memcmp(lv.iov_base, "parent-00001999", 15));
}

} /* namespace */
