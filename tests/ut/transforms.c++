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

#include <fcntl.h>
#include <unistd.h>

#include <string>

static std::string to_std_string(const mdbx::slice &s) {
  return s.as_string<char, std::char_traits<char>, std::allocator<char>>();
}

TEST(ut_transforms, key_value_transforms) {
  // int32: numerical order is preserved, roundtrip decodes correctly
  const uint32_t k1 = mdbx::key_from_int32(1);
  const uint32_t k10 = mdbx::key_from_int32(10);
  const uint32_t k2 = mdbx::key_from_int32(2);
  EXPECT_LT(k1, k2);
  EXPECT_LT(k2, k10);
  EXPECT_EQ(mdbx::int32_from_key(mdbx::slice(&k1, sizeof(k1))), 1);
  EXPECT_EQ(mdbx::int32_from_key(mdbx::slice(&k10, sizeof(k10))), 10);

  // int64 roundtrip
  const uint64_t i64 = mdbx::key_from_int64(-1234567890123ll);
  EXPECT_EQ(mdbx::int64_from_key(mdbx::slice(&i64, sizeof(i64))), -1234567890123ll);

  // double: ordering + roundtrip
  const uint64_t d1 = mdbx::key_from_double(-1.5);
  const uint64_t d2 = mdbx::key_from_double(0.0);
  const uint64_t d3 = mdbx::key_from_double(2.25);
  EXPECT_LT(d1, d2);
  EXPECT_LT(d2, d3);
  EXPECT_DOUBLE_EQ(mdbx::double_from_key(mdbx::slice(&d3, sizeof(d3))), 2.25);

  // float roundtrip
  const uint32_t f = mdbx::key_from_float(3.5f);
  EXPECT_FLOAT_EQ(mdbx::float_from_key(mdbx::slice(&f, sizeof(f))), 3.5f);

  // JSON integers are comparable with doubles
  const uint64_t j = mdbx::key_from_jsonInteger(42);
  EXPECT_EQ(mdbx::jsonInteger_from_key(mdbx::slice(&j, sizeof(j))), 42);
}

TEST(ut_transforms, dump_val) {
  EXPECT_EQ(mdbx::dump_val(mdbx::slice("hello")), "hello");
  EXPECT_EQ(mdbx::dump_val(mdbx::slice()), "<empty>");

  unsigned char binary[] = {0x00, 0x01, 0xFE, 0xFF};
  const std::string dumped = mdbx::dump_val(mdbx::slice(binary, sizeof(binary)));
  EXPECT_NE(dumped.find('<'), std::string::npos);
  EXPECT_NE(dumped.find('>'), std::string::npos);
  EXPECT_NE(dumped.find("fe"), std::string::npos);
}

TEST(ut_transforms, txn_copy) {
  const mdbx::path testdb = "test-transforms";
  const mdbx::path copydb = "test-transforms-copy";
  mdbx::env::remove(testdb);
  mdbx::env::remove(copydb);
  {
    mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto txn = env.start_write();
    auto table = txn.create_map("table-1");
    for (int i = 0; i < 100; ++i) {
      auto key = std::to_string(i);
      txn.upsert(table, mdbx::slice(key), mdbx::slice("value"));
    }
    txn.commit();

    // copy the consistent MVCC-snapshot of a read transaction
    auto reader = env.start_read();
    reader.copy(copydb.c_str(), /*compactify=*/true);
  }
  {
    mdbx::env_managed copyenv(copydb.c_str(), mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto txn = copyenv.start_read();
    auto table = txn.open_map("table-1");
    EXPECT_EQ(to_std_string(txn.get(table, mdbx::slice("0"))), "value");
    EXPECT_EQ(to_std_string(txn.get(table, mdbx::slice("99"))), "value");
  }
  mdbx::env::remove(copydb);
  mdbx::env::remove(testdb);
}

TEST(ut_transforms, txn_copy_fd) {
  const mdbx::path testdb = "test-transforms";
  const mdbx::path copydb = "test-transforms-copy-fd";
  mdbx::env::remove(testdb);
  mdbx::env::remove(copydb);
  {
    mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto txn = env.start_write();
    auto table = txn.create_map("table-1");
    txn.upsert(table, mdbx::slice("key"), mdbx::slice("value"));
    txn.commit();

    const int fd = ::open(copydb.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
    ASSERT_GE(fd, 0);
    auto reader = env.start_read();
    reader.copy(fd, /*compactify=*/false);
    ::close(fd);
  }
  {
    mdbx::env_managed copyenv(copydb.c_str(), mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
    auto txn = copyenv.start_read();
    auto table = txn.open_map("table-1");
    EXPECT_EQ(to_std_string(txn.get(table, mdbx::slice("key"))), "value");
  }
  mdbx::env::remove(copydb);
  mdbx::env::remove(testdb);
}

TEST(ut_transforms, cursor_count_ex) {
  const mdbx::path testdb = "test-transforms";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto table = txn.create_map("table-1", mdbx::key_mode::usual, mdbx::value_mode::multi);
    txn.upsert(table, mdbx::slice("key"), mdbx::slice("first"));
    mdbx::slice second("second");
    txn.put(table, mdbx::slice("key"), &second, MDBX_APPENDDUP);
    txn.commit();
  }

  auto txn = env.start_read();
  auto cursor = txn.open_cursor(txn.open_map("table-1", mdbx::key_mode::usual, mdbx::value_mode::multi));
  EXPECT_TRUE(cursor.lower_bound(mdbx::slice("key")));
  EXPECT_EQ(cursor.count_multivalue(), 2u);
  MDBX_stat stat{};
  EXPECT_EQ(cursor.count_multivalue(&stat), 2u);
  EXPECT_GT(stat.ms_entries, 0u);
  txn.abort();
  env.close();
  mdbx::env::remove(testdb);
}