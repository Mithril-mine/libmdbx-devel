/// \copyright SPDX-License-Identifier: Apache-2.0
///
/// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/306
///
/// mdbx_env_defrag() adopts the write transaction that is open on the calling
/// thread. A moved page that stays in the transaction's dirty list got the
/// committed txnid instead of front_txnid, so at the commit the page no
/// longer read as modifiable and was not queued for writing: the commit
/// returned MDBX_SUCCESS and the new meta named a root whose content never
/// reached the file. After a reopen most keys were gone and the tree was
/// broken. Now a moved dirty page keeps the txnid of a modifiable page.

#include "../env-guard.h++"
#include "mdbx.h"
#include <gtest/gtest.h>

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
  MDBX_env *release() {
    MDBX_env *e = env;
    env = nullptr;
    return e;
  }
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

TEST(issue_gh0306, defrag_inside_dirty_write_txn_commits_moved_pages) {
  const std::string db = "issue0306.mdbx";
  const env_files_guard guard({db.c_str()});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_geometry(env.get(), -1, INT64_C(1) << 20, INT64_C(1) << 28, INT64_C(1) << 20, -1, 4096),
            MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR, 0644), MDBX_SUCCESS);

  char val[120];
  memset(val, 'v', sizeof(val));
  char key[32];
  MDBX_dbi dbi = 0;

  /* the base fill and the half-deletion: retired pages for the defrag */
  txn_ptr txn;
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
  for (int i = 0; i < 4000; ++i) {
    snprintf(key, sizeof(key), "k%08d", i);
    MDBX_val k = {key, strlen(key)}, v = {val, 100};
    ASSERT_EQ(mdbx_put(txn.get(), dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
  for (int i = 0; i < 4000; i += 2) {
    snprintf(key, sizeof(key), "k%08d", i);
    MDBX_val k = {key, strlen(key)};
    ASSERT_EQ(mdbx_del(txn.get(), dbi, &k, nullptr), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
  for (int r = 0; r < 3; ++r) {
    ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_dbi_open(txn.get(), nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
    val[0] = static_cast<char>('a' + r);
    snprintf(key, sizeof(key), "k%08d", 3999);
    MDBX_val k = {key, strlen(key)}, v = {val, 100};
    ASSERT_EQ(mdbx_put(txn.get(), dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);
    ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
  }

  /* the write transaction holding dirty pages, then the defrag adopts it */
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
  val[0] = 'z';
  snprintf(key, sizeof(key), "k%08d", 1);
  MDBX_val k = {key, strlen(key)}, v = {val, 100};
  ASSERT_EQ(mdbx_put(txn.get(), dbi, &k, &v, MDBX_UPSERT), MDBX_SUCCESS);

  MDBX_defrag_result_t r;
  memset(&r, 0, sizeof(r));
  ASSERT_EQ(mdbx_env_defrag(env.get(), 0, 0, 0, 0, -1, 0, nullptr, nullptr, &r), MDBX_SUCCESS);
  ASSERT_GT(r.pages_moved, 0u) << "the scenario must actually move pages";

  /* the commit must write the moved dirty pages */
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_close(env.release()), MDBX_SUCCESS);

  /* reopen: all the remaining keys must be there */
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR | MDBX_RDONLY, 0644), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_txn_begin(env.get(), nullptr, MDBX_TXN_RDONLY, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
  size_t found = 0;
  for (int i = 1; i < 4000; i += 2) {
    snprintf(key, sizeof(key), "k%08d", i);
    MDBX_val rk = {key, strlen(key)}, rv;
    found += (mdbx_get(txn.get(), dbi, &rk, &rv) == MDBX_SUCCESS);
  }
  EXPECT_EQ(found, 2000u);
  snprintf(key, sizeof(key), "k%08d", 1);
  MDBX_val rk = {key, strlen(key)}, rv;
  ASSERT_EQ(mdbx_get(txn.get(), dbi, &rk, &rv), MDBX_SUCCESS);
  EXPECT_EQ(static_cast<const char *>(rv.iov_base)[0], 'z');
  mdbx_txn_abort(txn.release());
}

} /* namespace */
