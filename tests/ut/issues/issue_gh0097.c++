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

#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/97
// Seven C++ txn/env wrapper defects: amend() null-deref after commit,
// amend(true)/try_start_write() throwing MDBX_BUSY, getters built on
// get_stat() unusable inside a read transaction, flags() reporting a finished
// transaction as read-write, setup_debug() removing the application logger,
// and set_sync_period__seconds_double() converting out-of-range values with
// UB. The read-transaction cases are exercised here.

TEST(issue_gh0097, getters_usable_inside_read_transaction) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue97.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  /* get_pagesize() and the limits built on it must work while the thread
   * holds a read transaction (they used to start their own read txn and
   * throw reader_slot_busy). The actual page size is platform-dependent */
  {
    const size_t pagesize_outside = env.get_pagesize();
    mdbx::txn_managed rd = env.start_read();
    EXPECT_EQ(pagesize_outside, env.get_pagesize()) << "get_pagesize() must work inside a read txn";
    EXPECT_GT(env.get_pagesize(), (size_t)0);
    EXPECT_GT(env.dbsize_max(), (size_t)0);
    EXPECT_GT(env.transaction_size_max(), (size_t)0);
  }

  /* txn::flags() after abort() must throw MDBX_BAD_TXN (it used to compare
   * against -1 and report INT32_MIN as a live read-write transaction). */
  mdbx::txn_managed w = env.start_write();
  w.abort();
  EXPECT_THROW(w.is_readwrite(), mdbx::bad_transaction) << "a finished transaction must not report read-write";

  /* amend(dont_wait=true) must not dereference a null handle after commit. */
  {
    mdbx::txn_managed t = env.start_write();
    t.commit();
    EXPECT_THROW(t.amend(), mdbx::bad_transaction) << "amend() after commit must throw, not crash";
  }

  env.close();
  mdbx::env::remove(testdb);
}

TEST(issue_gh0097, sync_period_double_rejects_out_of_range) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue97b.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  EXPECT_THROW(env.set_sync_period__seconds_double(86400), std::invalid_argument)
      << "a period beyond the 16.16 range must be rejected, not converted with UB";
  env.set_sync_period__seconds_double(60);
  EXPECT_DOUBLE_EQ(60.0, env.sync_period__seconds_double());

  env.close();
  mdbx::env::remove(testdb);
}