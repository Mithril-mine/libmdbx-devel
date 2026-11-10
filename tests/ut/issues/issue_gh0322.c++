/// \copyright SPDX-License-Identifier: Apache-2.0
///
/// Regression tests for https://github.com/Mithril-mine/libmdbx-devel/issues/322
///
/// The pre-sync of data pages in env_sync() and the meta_sync() calls from
/// env_sync() and txn_basal_commit() returned a failed flush to the caller
/// but left the environment usable, unlike every other flush on the commit
/// and sync path (ENV_FATAL_ERROR, then MDBX_PANIC). On Linux a writeback
/// error is reported once and the pages that failed may be dropped, so a
/// retry of mdbx_env_sync() succeeded and signed a steady meta page over
/// data that never reached the disk (or recorded a meta page as synced when
/// it was not). Now all these paths mark the environment fatal, as
/// dxb_sync_locked() already did.
///
/// Requires a probes-enabled build: the flush failures are injected via the
/// probe-bus fault sites.

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

/* commits 2000 records, so the following sync has something to flush */
void put_some(MDBX_env *env) {
  txn_ptr txn;
  MDBX_dbi dbi = 0;
  char kbuf[16], vbuf[100];
  MDBX_val key = {kbuf, 0}, val = {vbuf, sizeof(vbuf)};
  memset(vbuf, 'v', sizeof(vbuf));
  ASSERT_EQ(mdbx_txn_begin(env, NULL, MDBX_TXN_READWRITE, &txn), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_dbi_open(txn.get(), nullptr, MDBX_DB_DEFAULTS, &dbi), MDBX_SUCCESS);
  for (unsigned i = 0; i < 2000; ++i) {
    key.iov_len = (size_t)snprintf(kbuf, sizeof(kbuf), "%08u", i);
    ASSERT_EQ(mdbx_put(txn.get(), dbi, &key, &val, MDBX_UPSERT), MDBX_SUCCESS);
  }
  ASSERT_EQ(mdbx_txn_commit(txn.release()), MDBX_SUCCESS);
}

/* Item 1: a failed pre-sync of data pages must make the environment fatal,
 * so a retry cannot sign a steady meta over the lost pages. */
TEST(issue_gh0322, pp1_failed_presync_marks_env_fatal) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0322a.mdbx";
  const env_files_guard guard({db.c_str()});
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR | MDBX_SAFE_NOSYNC, 0644), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_option(env.get(), MDBX_opt_presync_threshold, 0), MDBX_SUCCESS);
  ASSERT_NO_FATAL_FAILURE(put_some(env.get()));

  char reply[256];
  ASSERT_EQ(0, mdbx_probe_ctl("fault presync_data_flush_err 5", reply, sizeof reply));
  EXPECT_EQ(mdbx_env_sync_ex(env.get(), true, false), MDBX_EIO);
  ASSERT_EQ(0, mdbx_probe_ctl("fault presync_data_flush_err none", reply, sizeof reply));

  /* the environment must be fatal: a retry reports MDBX_PANIC and attempts
   * no flush, instead of reporting success over the lost pages */
  EXPECT_EQ(mdbx_env_sync_ex(env.get(), true, false), MDBX_PANIC);
  EXPECT_EQ(mdbx_env_sync_ex(env.get(), true, false), MDBX_PANIC);
  EXPECT_EQ(mdbx_env_close(env.release()), MDBX_SUCCESS);
}

/* Item 2: a failed flush of the meta pages must make the environment fatal,
 * so a retry cannot record a meta page as synced when it was not. */
TEST(issue_gh0322, pp2_failed_meta_sync_marks_env_fatal) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string db = "issue0322c.mdbx";
  const env_files_guard guard({db.c_str()});
  env_ptr env;
  ASSERT_EQ(mdbx_env_create(&env), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_open(env.get(), db.c_str(), MDBX_NOSUBDIR | MDBX_WRITEMAP | MDBX_NOMETASYNC, 0644), MDBX_SUCCESS);
  ASSERT_EQ(mdbx_env_set_option(env.get(), MDBX_opt_presync_threshold, 0), MDBX_SUCCESS);
  ASSERT_NO_FATAL_FAILURE(put_some(env.get()));

  char reply[256];
  ASSERT_EQ(0, mdbx_probe_ctl("fault meta_sync_flush_err 5", reply, sizeof reply));
  EXPECT_EQ(mdbx_env_sync_ex(env.get(), true, false), MDBX_EIO);
  ASSERT_EQ(0, mdbx_probe_ctl("fault meta_sync_flush_err none", reply, sizeof reply));

  /* the environment must be fatal: a retry reports MDBX_PANIC and attempts
   * no flush, instead of recording the meta page as synced */
  EXPECT_EQ(mdbx_env_sync_ex(env.get(), true, false), MDBX_PANIC);
  EXPECT_EQ(mdbx_env_sync_ex(env.get(), true, false), MDBX_PANIC);
  EXPECT_EQ(mdbx_env_close(env.release()), MDBX_SUCCESS);
}

} /* namespace */
