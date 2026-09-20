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

namespace {
void fill(mdbx::txn_managed &txn, mdbx::map_handle map, size_t count = 1000) {
  for (size_t i = 0; i < count; ++i) {
    char key[16], val[16];
    snprintf(key, sizeof(key), "%08zu", i);
    snprintf(val, sizeof(val), "v%06zu", i);
    txn.upsert(map, mdbx::slice(key, strlen(key)), mdbx::slice(val, strlen(val)));
  }
}
} // namespace

TEST(ut_cursor_api, navigation_and_estimate) {
  const mdbx::path testdb = "test-cursor-api";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("table");
    fill(txn, map);

    auto cur = txn.open_cursor(map);
    EXPECT_EQ(cur.to_first().key, mdbx::slice("00000000"));
    EXPECT_EQ(cur.to_last().key, mdbx::slice("00000999"));
    cur.to_first();
    EXPECT_EQ(cur.to_next().key, mdbx::slice("00000001"));
    EXPECT_EQ(cur.to_previous().key, mdbx::slice("00000000"));

    EXPECT_NO_THROW((void)cur.estimate(mdbx::cursor::move_operation::first));
    EXPECT_NO_THROW((void)cur.estimate(mdbx::cursor::move_operation::last));

    cur.close();
    txn.commit();
  }
  {
    auto txn = env.start_read();
    auto map = txn.open_map("table");
    auto cur = txn.open_cursor(map);
    EXPECT_EQ(cur.lower_bound(mdbx::slice("00000500")).key, mdbx::slice("00000500"));
    EXPECT_EQ(cur.move(mdbx::cursor::move_operation::next, true).key, mdbx::slice("00000501"));
    cur.close();
    txn.abort();
  }
  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_cursor_api, count_multivalue) {
  const mdbx::path testdb = "test-cursor-api";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  auto txn = env.start_write();
  auto map = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);
  for (int i = 0; i < 5; ++i) {
    char val[8];
    snprintf(val, sizeof(val), "v%d", i);
    txn.upsert(map, "key", val);
  }
  auto cur = txn.open_cursor(map);
  cur.to_first();
  EXPECT_EQ(cur.count_multivalue(), 5u);
  cur.close();
  txn.commit();
  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_map_ops, lifecycle) {
  const mdbx::path testdb = "test-cursor-api";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto a = txn.create_map("a");
    auto b = txn.create_map("b", mdbx::key_mode::usual, mdbx::value_mode::multi);
    EXPECT_NE(a.dbi, b.dbi);

    txn.upsert(a, "k", "v");
    EXPECT_THROW(txn.open_map("absent"), mdbx::not_found);
    auto accede = txn.open_map_accede("a");
    EXPECT_EQ(accede.dbi, a.dbi);

    txn.rename_map(a, "a-renamed");
    EXPECT_EQ(txn.open_map("a-renamed").dbi, a.dbi);
    EXPECT_THROW(txn.open_map("a"), mdbx::not_found);

    txn.drop_map(a);
    EXPECT_THROW(txn.open_map("a-renamed"), mdbx::not_found);

    size_t enumerated = 0;
    auto visitor = [&](const mdbx::slice &, MDBX_db_flags_t, const MDBX_stat &, MDBX_dbi) {
      ++enumerated;
      return true;
    };
    txn.enumerate_tables(visitor);
    EXPECT_EQ(enumerated, 1u) << "table b is still present after dropping a";
    EXPECT_TRUE(txn.drop_map("b", /*throw_if_absent=*/false));
    EXPECT_FALSE(txn.drop_map("b", /*throw_if_absent=*/false));

    txn.commit();
  }
  env.close();
  mdbx::env::remove(testdb);
}