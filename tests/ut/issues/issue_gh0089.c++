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

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/89
// txn_managed::checkpoint() reset handle_ when mdbx_txn_checkpoint() returned
// MDBX_BAD_TXN, but that code comes from the entry state check and leaves the
// write transaction live: the C++ object then lost ownership, its destructor
// did not abort the transaction, and the writer lock was never released, so
// every later writer failed with MDBX_BUSY. The handle must be kept for
// errors that do not change the transaction state, so the destructor still
// aborts the broken transaction.

TEST(issue_gh0089, checkpoint_badtxn_keeps_handle) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue89.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  {
    mdbx::txn_managed txn = env.start_write();
    auto map = txn.create_map("t");
    txn.upsert(map, mdbx::slice("k"), mdbx::slice("v"));
    /* A broken write transaction can never be checkpointed (MDBX_BAD_TXN from
     * the entry state check), but the transaction itself stays live. */
    ::mdbx_txn_break(txn.handle());
    EXPECT_THROW(txn.checkpoint(), mdbx::bad_transaction);
    EXPECT_NE(nullptr, txn.handle()) << "checkpoint() failure must not drop ownership";
  }

  /* The destructor must have aborted the broken transaction, releasing the
   * writer lock: a dont-wait writer must now succeed. */
  mdbx::txn_managed second = env.start_write(true /* dont_wait */);
  second.abort();

  env.close();
  mdbx::env::remove(testdb);
}