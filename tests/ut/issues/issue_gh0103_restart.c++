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
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdio>
#include <cstring>

extern "C" int mprobe_ctl(const char *request, char *reply, size_t reply_size);

// Follow-up regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/103
// mdbx_txn_rollback() of a nested write transaction undoes it and starts it
// again. When the restart failed (MDBX_ENOMEM), the nested transaction was
// aborted, which undid it a second time: the parent's retired-page list got a
// pointer value as its length, so the parent's commit crashed (release), and
// the consistency checks of the abort failed on the half-restarted nested
// transaction (debug). The out-of-memory is injected via the probe-bus
// `alloc-fault`, so this requires MDBX_PROBES=ON and MDBX_PROBES=1.

TEST(issue_gh0103, failed_restart_keeps_parent_usable) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const dbname = "issue103-restart.mdbx";
  remove(dbname);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, dbname, MDBX_NOSUBDIR, 0644));

  MDBX_txn *parent = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &parent));
  MDBX_dbi main_dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(parent, nullptr, MDBX_DB_DEFAULTS, &main_dbi));
  char kbuf[16];
  MDBX_val key, data = {(void *)"value", 5};
  for (int i = 0; i < 200; ++i) {
    key = {kbuf, size_t(std::snprintf(kbuf, sizeof kbuf, "k%05d", i))};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(parent, main_dbi, &key, &data, MDBX_UPSERT));
  }

  /* The nested transaction retires pages, so the retired-page list is handed
   * over to it and must come back to the parent intact. */
  MDBX_txn *nested = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, parent, MDBX_TXN_READWRITE, &nested));
  for (int i = 0; i < 100; ++i) {
    key = {kbuf, size_t(std::snprintf(kbuf, sizeof kbuf, "k%05d", i))};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_del(nested, main_dbi, &key, nullptr));
  }

  char reply[256];
  ASSERT_EQ(0, mprobe_ctl("alloc-fault 1", reply, sizeof reply));
  const int rc = mdbx_txn_rollback(nested);
  ASSERT_EQ(0, mprobe_ctl("alloc-fault none", reply, sizeof reply));
  ASSERT_EQ(MDBX_ENOMEM, rc) << "the restart must fail with the injected out-of-memory";

  /* The nested transaction is gone; the parent must be intact and commit. */
  key = {(void *)"zz", 2};
  ASSERT_EQ(MDBX_SUCCESS, mdbx_put(parent, main_dbi, &key, &data, MDBX_UPSERT));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(parent));

  MDBX_txn *reader = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &reader));
  MDBX_stat st;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(reader, main_dbi, &st, sizeof(st)));
  EXPECT_EQ(201u, st.ms_entries) << "the parent's 200 keys and \"zz\", without the rolled-back deletions";
  mdbx_txn_abort(reader);

  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
  remove(dbname);
}
