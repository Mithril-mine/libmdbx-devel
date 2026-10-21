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
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/102
// C++ wrappers passed documented non-error results to success_or_throw(),
// throwing std::logic_error("MDBX_RESULT_TRUE") instead of an mdbx::exception
// or a normal result: thread_register/unregister on a (un)registered thread,
// get_tree_deepmask() on a non-multi-value table, sequence() on overflow, and
// commit() of a transaction that had an error; defrag() threw
// MDBX_LAGGARD_READER instead of reporting it in the result.

TEST(issue_gh0102, thread_register_returns_false_when_already_registered) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue102.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  /* A thread not yet registered: the first register assigns a slot. */
  EXPECT_TRUE(env.thread_register()) << "the first register assigns a slot";
  EXPECT_FALSE(env.thread_register()) << "the second register reports already-registered, not an exception";
  EXPECT_TRUE(env.thread_unregister()) << "the first unregister releases the slot";
  EXPECT_FALSE(env.thread_unregister()) << "the second unregister reports not-registered, not an exception";

  env.close();
  mdbx::env::remove(testdb);
}

TEST(issue_gh0102, non_error_results_do_not_throw) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue102b.mdbx";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));

  mdbx::txn_managed txn = env.start_write();
  auto plain = txn.create_map("plain");
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);
  txn.upsert(multi, mdbx::slice("k"), mdbx::slice("v1"));
  txn.upsert(multi, mdbx::slice("k"), mdbx::slice("v2"));

  /* A non-multi-value table has no nested trees: mask is 0, not an exception;
   * a multi-value table with duplicates reports a non-zero mask. */
  EXPECT_EQ(0u, txn.get_tree_deepmask(plain)) << "a non-multi-value table must report mask 0";
  EXPECT_NE(0u, txn.get_tree_deepmask(multi)) << "a multi-value table must report a non-zero mask";
  txn.abort();

  /* commit() of a broken transaction must throw mdbx::bad_transaction, not
   * std::logic_error("MDBX_RESULT_TRUE"). */
  mdbx::txn_managed broken = env.start_write();
  auto bmap = broken.create_map("bmap");
  broken.upsert(bmap, mdbx::slice("b"), mdbx::slice("v"));
  broken.make_broken();
  EXPECT_THROW(broken.commit(), mdbx::bad_transaction);

  env.close();
  mdbx::env::remove(testdb);
}