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

#include <algorithm>
#include <string>
#include <vector>

TEST(ut_tables_and_chk, enumerate_tables) {
  const mdbx::path testdb = "test-tables-and-chk";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(16));
  {
    auto txn = env.start_write();
    auto t1 = txn.create_map("table-1");
    auto t2 = txn.create_map("table-2", mdbx::key_mode::ordinal, mdbx::value_mode::multi);
    txn.upsert(t1, mdbx::slice("key"), mdbx::slice("value"));
    txn.insert(t2, mdbx::slice::wrap(1u), mdbx::slice("first"));
    txn.insert(t2, mdbx::slice::wrap(2u), mdbx::slice("second"));
    txn.commit();
  }

  using plain_string = std::basic_string<char, std::char_traits<char>, std::allocator<char>>;
  {
    auto txn = env.start_read();
    std::vector<plain_string> names;
    std::vector<MDBX_db_flags_t> flags;
    auto visitor = [&](const mdbx::slice &name, MDBX_db_flags_t f, const MDBX_stat &stat, MDBX_dbi) {
      names.push_back(name.as_string<char, std::char_traits<char>, std::allocator<char>>());
      flags.push_back(f);
      (void)stat;
      return mdbx::loop_control::continue_loop;
    };
    int rc = txn.enumerate_tables(visitor);
    EXPECT_EQ(rc, 0);
    ASSERT_EQ(names.size(), 2u);
    EXPECT_NE(std::find(names.begin(), names.end(), "table-1"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "table-2"), names.end());
    const auto flags_of_table2 = [&]() -> MDBX_db_flags_t {
      for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == "table-2")
          return flags[i];
      return MDBX_db_flags_t(0);
    }();
    EXPECT_NE(MDBX_db_flags_t(0), flags_of_table2 & MDBX_INTEGERKEY);
    EXPECT_NE(MDBX_db_flags_t(0), flags_of_table2 & MDBX_DUPSORT);
  }

  // exit_loop aborts enumeration early
  {
    auto txn = env.start_read();
    auto visitor = [](const mdbx::slice &, MDBX_db_flags_t, const MDBX_stat &, MDBX_dbi) {
      return mdbx::loop_control::exit_loop;
    };
    int rc = txn.enumerate_tables(visitor);
    EXPECT_EQ(rc, mdbx::loop_control::exit_loop);
  }
  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_tables_and_chk, env_chk) {
  const mdbx::path testdb = "test-tables-and-chk";
  mdbx::env::remove(testdb);
  {
    mdbx::env_managed env(testdb,
                          mdbx::create_parameters().set_geometry(mdbx::geometry().make_fixed(1 * mdbx::geometry::MiB)),
                          mdbx::operate_parameters().set_max_maps(16));
    {
      auto txn = env.start_write();
      auto table = txn.create_map("table-1");
      for (int i = 0; i < 128; ++i) {
        auto key = std::to_string(i);
        txn.upsert(table, mdbx::slice(key), mdbx::slice("value"));
      }
      txn.commit();
    }

    MDBX_chk_context_t ctx{};
    const MDBX_chk_context_t &result = env.chk(ctx);
    EXPECT_EQ(&result, &ctx);
    EXPECT_EQ(ctx.result.total_problems, 0u);
    EXPECT_GE(ctx.result.table_total, 1u);
    EXPECT_EQ(ctx.result.steady_txnid, ctx.result.recent_txnid);
  }
  mdbx::env::remove(testdb);
}