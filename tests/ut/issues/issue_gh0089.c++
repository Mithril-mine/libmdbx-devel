#include "mdbx.h++"
#include <gtest/gtest.h>

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