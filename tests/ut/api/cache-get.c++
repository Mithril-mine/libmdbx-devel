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

static std::string to_std_string(const mdbx::slice &s) {
  return s.as_string<char, std::char_traits<char>, std::allocator<char>>();
}

TEST(ut_cache_get, cache_entry_type) {
  mdbx::cache_entry entry;
  EXPECT_FALSE(entry); // freshly initialized: no confirmed result
  EXPECT_TRUE(entry.notfound()); // offset == 0 is the "not found" marker

  mdbx::cache_entry copy(entry);
  EXPECT_TRUE(copy == entry);
  mdbx::cache_entry moved(std::move(copy));
  EXPECT_TRUE(moved == entry);
  EXPECT_FALSE(copy); // moved-from is reset
  EXPECT_TRUE(copy == mdbx::cache_entry());

  moved.reset();
  EXPECT_FALSE(moved);
  EXPECT_TRUE(moved == mdbx::cache_entry());
}

TEST(ut_cache_get, get_cached) {
  const mdbx::path testdb = "test-cache-get";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto table = txn.create_map("table-1");
    txn.upsert(table, mdbx::slice("key-1"), mdbx::slice("value-1"));
    txn.upsert(table, mdbx::slice("key-2"), mdbx::slice("value-2"));
    txn.commit();
  }

  mdbx::cache_entry entry_a, entry_b; // one entry per key
  {
    auto txn = env.start_read();
    auto table = txn.open_map("table-1");

    // faithful variant fills the entry and returns the cached value
    mdbx::slice data;
    mdbx::txn::cache_result result = txn.get_cached(table, mdbx::slice("key-1"), &data, entry_a);
    EXPECT_EQ(result.errcode, MDBX_SUCCESS);
    EXPECT_TRUE(entry_a);
    EXPECT_FALSE(entry_a.notfound());
    EXPECT_EQ(to_std_string(data), "value-1");

    // repeated lookups are correct (cached fast path)
    for (int i = 0; i < 8; ++i) {
      mdbx::slice again;
      result = txn.get_cached(table, mdbx::slice("key-1"), &again, entry_a);
      EXPECT_EQ(result.errcode, MDBX_SUCCESS);
      EXPECT_EQ(to_std_string(again), "value-1");
    }

    // convenient variant
    mdbx::txn::cache_status status;
    mdbx::slice value = txn.get_cached(table, mdbx::slice("key-2"), entry_b, &status);
    EXPECT_EQ(to_std_string(value), "value-2");
    EXPECT_TRUE(entry_b);

    // missing key: faithful reports NOTFOUND and caches it; convenient throws
    mdbx::cache_entry entry_miss;
    mdbx::slice miss;
    result = txn.get_cached(table, mdbx::slice("no-such-key"), &miss, entry_miss);
    EXPECT_EQ(result.errcode, MDBX_NOTFOUND);
    EXPECT_TRUE(entry_miss);
    EXPECT_TRUE(entry_miss.notfound());
    EXPECT_THROW((void)txn.get_cached(table, mdbx::slice("no-such-key"), entry_miss), mdbx::not_found);
  }

  // update the value and verify the cache invalidates correctly
  {
    auto txn = env.start_write();
    auto table = txn.open_map("table-1");
    txn.upsert(table, mdbx::slice("key-1"), mdbx::slice("value-1-updated"));
    txn.commit();
  }
  {
    auto txn = env.start_read();
    auto table = txn.open_map("table-1");
    mdbx::slice data;
    mdbx::txn::cache_result result = txn.get_cached(table, mdbx::slice("key-1"), &data, entry_a);
    EXPECT_EQ(result.errcode, MDBX_SUCCESS);
    EXPECT_EQ(to_std_string(data), "value-1-updated");
  }

  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_cache_get, open_for_recovery) {
  const mdbx::path testdb = "test-cache-get";
  mdbx::env::remove(testdb);
  {
    mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto txn = env.start_write();
    auto table = txn.create_map("table-1");
    txn.upsert(table, mdbx::slice("key"), mdbx::slice("value"));
    txn.commit();
  }

  auto recovered = mdbx::env_managed::open_for_recovery(testdb.c_str(), 0, /*writeable=*/true);
  MDBX_chk_context_t ctx{};
  const MDBX_chk_context_t &chk_result = recovered.chk(ctx);
  EXPECT_EQ(&chk_result, &ctx);
  EXPECT_EQ(ctx.result.total_problems, 0u);
  EXPECT_NO_THROW(recovered.turn_for_recovery(0));
  recovered.close();
  mdbx::env::remove(testdb);
}