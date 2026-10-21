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
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdio>
#include <string>

static std::string to_std_string(const mdbx::slice &s) {
  return s.as_string<char, std::char_traits<char>, std::allocator<char>>();
}

TEST(ut_cursor_batch, get_batch) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-cursor-batch";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto table = txn.create_map("table-1");
    char key[3];
    for (int i = 0; i < 25; ++i) {
      snprintf(key, sizeof(key), "%02d", i); // lexicographic == numeric order
      txn.upsert(table, mdbx::slice(key), mdbx::slice("value"));
    }
    txn.commit();
  }

  {
    auto txn = env.start_read();
    auto cursor = txn.open_cursor(txn.open_map("table-1"));

    bool is_last = false;
    auto batch = cursor.get_batch(10, mdbx::cursor::move_operation::first, &is_last);
    ASSERT_EQ(batch.size(), 10u);
    EXPECT_FALSE(is_last);
    EXPECT_EQ(to_std_string(batch.front().key), "00");
    EXPECT_EQ(to_std_string(batch.back().key), "09");

    batch = cursor.get_batch(10, mdbx::cursor::move_operation::next, &is_last);
    ASSERT_EQ(batch.size(), 10u);
    EXPECT_FALSE(is_last);
    EXPECT_EQ(to_std_string(batch.back().key), "19");

    batch = cursor.get_batch(10, mdbx::cursor::move_operation::next, &is_last);
    ASSERT_EQ(batch.size(), 5u);
    EXPECT_TRUE(is_last);
    EXPECT_EQ(to_std_string(batch.back().key), "24");
  }
  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_cursor_batch, ignore_key_order) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-cursor-batch";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto table = txn.create_map("table-1");
    txn.upsert(table, mdbx::slice("k"), mdbx::slice("v"));
    txn.commit();
  }

  auto txn = env.start_read();
  auto cursor = txn.open_cursor(txn.open_map("table-1"));
  EXPECT_NO_THROW(cursor.ignore_key_order());
  auto pair = cursor.to_first();
  EXPECT_TRUE(pair);
  txn.abort();
  env.close();
  mdbx::env::remove(testdb);
}