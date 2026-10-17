#include "mdbx.h++"
#include <gtest/gtest.h>

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