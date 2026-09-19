/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// THE CONTENTS OF THIS PROJECT ARE PROPRIETARY AND CONFIDENTIAL.
/// UNAUTHORIZED COPYING, TRANSFERRING OR REPRODUCTION OF THE CONTENTS OF THIS PROJECT,
/// VIA ANY MEDIUM IS STRICTLY PROHIBITED.
///
/// The receipt or possession of the source code and/or any parts thereof does not convey or imply any right to use them
/// for any purpose other than the purpose for which they were provided to you.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software
/// or the use or other dealings in the software.
///
/// The above copyright notice and this permission notice shall be included in all copies
/// or substantial portions of the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>
/// \date 2015-2026

#include "mdbx.h++"
#include <gtest/gtest.h>

#include <string>

TEST(ut_stat_info, sysraminfo) {
  const mdbx::env::sysraminfo info = mdbx::env::get_sysraminfo();
  EXPECT_GT(info.page_size, intptr_t(0));
  EXPECT_LT(info.page_size, intptr_t(1 << 20));
  EXPECT_GT(info.total_pages, intptr_t(0));
  EXPECT_GE(info.avail_pages, intptr_t(0));
  EXPECT_LE(info.avail_pages, info.total_pages);
}

TEST(ut_stat_info, ratio_and_readahead) {
  EXPECT_NE(mdbx::ratio2percents(1, 4).find("25"), std::string::npos);
  EXPECT_NE(mdbx::ratio2percents(1, 3).find("33"), std::string::npos);
  EXPECT_NE(mdbx::ratio2digits(1, 4, 2).find("0.25"), std::string::npos);
  EXPECT_FALSE(mdbx::ratio2digits(355, 113, 3).empty());
  // just must be callable
  (void)mdbx::is_readahead_reasonable(1u << 30, 0);
}

TEST(ut_stat_info, preopen_snapinfo) {
  const mdbx::path testdb = "test-stat-info";
  mdbx::env::remove(testdb);
  {
    mdbx::env_managed env(testdb, mdbx::create_parameters(),
                          mdbx::operate_parameters().set_max_maps(8));
    auto txn = env.start_write();
    auto table = txn.create_map("table-1");
    txn.upsert(table, mdbx::slice("key"), mdbx::slice("value"));
    txn.commit();
  }

  const mdbx::env::info snap = mdbx::env::get_preopen_snapinfo(testdb.c_str());
  EXPECT_GT(snap.mi_dxb_pagesize, uint32_t(0));
  EXPECT_NE(snap.mi_recent_txnid, uint64_t(0));

  mdbx::env::remove(testdb);
}

TEST(ut_stat_info, gc_info) {
  const mdbx::path testdb = "test-stat-info";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto table = txn.create_map("table-1");
    for (int i = 0; i < 256; ++i) {
      auto key = std::to_string(i);
      txn.upsert(table, mdbx::slice(key), mdbx::slice("value-value-value"));
    }
    for (int i = 0; i < 256; i += 2)
      txn.erase(table, mdbx::slice(std::to_string(i)));
    txn.commit();
  }

  // GC is populated by the retired pages of the erased records
  auto txn = env.start_read();
  const mdbx::txn::gc_info info = txn.get_gc_info();
  EXPECT_GT(info.pages_allocated, size_t(0));
  EXPECT_GT(info.pages_total, info.pages_allocated);

  size_t spans = 0;
  auto visitor = [&](uint64_t span_txnid, size_t span_pgno, size_t span_length, bool reclaimable) {
    EXPECT_NE(span_txnid, uint64_t(0));
    EXPECT_GE(span_length, size_t(1));
    (void)span_pgno;
    (void)reclaimable;
    ++spans;
    return mdbx::loop_control::continue_loop;
  };
  const mdbx::txn::gc_info info2 = txn.get_gc_info(visitor);
  EXPECT_GT(spans, size_t(0));
  EXPECT_EQ(info2.pages_gc, info.pages_gc);
  txn.abort();
  env.close();
  mdbx::env::remove(testdb);
}
TEST(ut_stat_info, debug_and_threads) {
  EXPECT_GE(mdbx::setup_debug(), 0); // read current settings, keep them
  mdbx::set_panic(nullptr);

  const mdbx::path testdb = "test-stat-info";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  EXPECT_NO_THROW(env.thread_register());
  EXPECT_NO_THROW(env.thread_unregister());
  EXPECT_NO_THROW(env.resurrect_after_fork());
  const int warmup_rc = env.warmup(MDBX_warmup_default, 0);
  EXPECT_TRUE(warmup_rc == MDBX_SUCCESS || warmup_rc == MDBX_RESULT_TRUE);
  env.close();
  mdbx::env::remove(testdb);
}
