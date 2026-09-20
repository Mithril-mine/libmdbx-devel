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

#include <cstring>

namespace {
mdbx::env_managed make_env(const char *name) {
  const mdbx::path testdb = name;
  mdbx::env::remove(testdb);
  return mdbx::env_managed(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
}
} // namespace

TEST(ut_put_flags, reserve_variants) {
  auto env = make_env("test-put-flags");
  auto txn = env.start_write();
  auto map = txn.create_map("table");

  {
    auto reservation = txn.insert_reserve(map, "k1", 4);
    ASSERT_EQ(reservation.size(), 4u);
    memcpy(reservation.byte_ptr(), "abcd", 4);
    EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("abcd"));
  }

  {
    const auto duplicate = txn.try_insert_reserve(map, "k1", 8);
    EXPECT_FALSE(duplicate.done);
    EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("abcd")) << "existing value must survive a failed reserve";
  }

  {
    auto reservation = txn.upsert_reserve(map, "k1", 7);
    ASSERT_EQ(reservation.size(), 7u);
    memcpy(reservation.byte_ptr(), "abcdefg", 7);
    EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("abcdefg"));
  }

  {
    auto reservation = txn.update_reserve(map, "k1", 3);
    ASSERT_EQ(reservation.size(), 3u);
    memcpy(reservation.byte_ptr(), "XYZ", 3);
    EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("XYZ"));
  }

  EXPECT_THROW((void)txn.update_reserve(map, "absent", 5), mdbx::not_found);

  {
    const auto missed = txn.try_update_reserve(map, "absent", 5);
    EXPECT_FALSE(missed.done);
    EXPECT_THROW(txn.get(map, "absent"), mdbx::not_found);
  }

  txn.commit();
  env.close();
  mdbx::env::remove("test-put-flags");
}

TEST(ut_put_flags, reserve_forbidden_on_dupsort) {
  auto env = make_env("test-put-flags");
  auto txn = env.start_write();
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);
  txn.upsert(multi, "k", "v");
  EXPECT_THROW((void)txn.insert_reserve(multi, "other", 4), mdbx::incompatible_operation)
      << "MDBX_RESERVE on DUPSORT is not permitted";
  txn.abort();
  env.close();
  mdbx::env::remove("test-put-flags");
}

TEST(ut_put_flags, append_single_and_dupsort) {
  auto env = make_env("test-put-flags");
  auto txn = env.start_write();
  auto map = txn.create_map("table");
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);

  txn.append(map, "a", "1");
  txn.append(map, "b", "2");
  txn.append(map, "c", "3");
  EXPECT_EQ(txn.get(map, "b"), mdbx::slice("2"));
  EXPECT_THROW(txn.append(map, "0", "zero"), mdbx::key_mismatch) << "out-of-order MDBX_APPEND is EKEYMISMATCH";
  EXPECT_THROW(txn.append(map, "b", "two"), mdbx::key_mismatch)
      << "MDBX_APPEND to an existing non-last key is EKEYMISMATCH";

  txn.append(multi, "k", "v1");
  txn.append(multi, "k", "v2");
  txn.append(multi, "k", "v3");
  EXPECT_EQ(txn.get_map_stat(multi).ms_entries, 3u);

  {
    mdbx::slice extra("v4");
    txn.put(multi, mdbx::slice("k"), &extra, MDBX_APPENDDUP);
    EXPECT_EQ(txn.get_map_stat(multi).ms_entries, 4u);
  }
  {
    mdbx::slice fresh("v5");
    txn.put(multi, mdbx::slice("k"), &fresh, MDBX_NODUPDATA);
    EXPECT_EQ(txn.get_map_stat(multi).ms_entries, 5u) << "NODUPDATA accepts a brand-new value";
  }
  {
    mdbx::slice present("v4");
    EXPECT_EQ(txn.put(multi, mdbx::slice("k"), &present, MDBX_NODUPDATA), MDBX_KEYEXIST)
        << "NODUPDATA rejects an already-present value";
  }

  auto cursor = txn.open_cursor(multi);
  cursor.to_first();
  size_t seen = 0;
  do {
    ++seen;
  } while (cursor.to_next(false));
  EXPECT_EQ(seen, 5u);

  txn.commit();
  env.close();
  mdbx::env::remove("test-put-flags");
}

TEST(ut_put_flags, append_to_dupsort_last_key) {
  auto env = make_env("test-put-flags");
  auto txn = env.start_write();
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);

  txn.append(multi, "k", "v1", /*multivalue_order_preserved=*/false);
  txn.append(multi, "m", "w1", /*multivalue_order_preserved=*/false);
  txn.append(multi, "m", "w2", /*multivalue_order_preserved=*/false);
  EXPECT_EQ(txn.get_map_stat(multi).ms_entries, 3u)
      << "plain MDBX_APPEND appends a dup to the LAST key of a dupsort table";
  EXPECT_THROW(txn.append(multi, "k", "x", /*multivalue_order_preserved=*/false), mdbx::key_mismatch)
      << "MDBX_APPEND to a non-last key is EKEYMISMATCH";

  txn.commit();
  env.close();
  mdbx::env::remove("test-put-flags");
}

TEST(ut_put_flags, put_multiple_samelength_modes) {
  auto env = make_env("test-put-flags");
  auto txn = env.start_write();
  auto table = txn.create_map("table", mdbx::key_mode::usual, mdbx::value_mode::multi_samelength);

  const uint32_t data[8] = {11, 12, 13, 14, 15, 16, 17, 18};

  EXPECT_EQ(txn.put_multiple_samelength(table, "k1", data, 4, mdbx::insert_unique), 4u);
  EXPECT_EQ(txn.get_map_stat(table).ms_entries, 4u);

  EXPECT_EQ(txn.put_multiple_samelength(table, "k2", data, 4, mdbx::upsert), 4u);

  EXPECT_EQ(txn.put_multiple_samelength(table, "k2", data + 6, 2, mdbx::update), 2u)
      << "MDBX_CURRENT|MDBX_MULTIPLE replaces the whole dup set";
  EXPECT_EQ(txn.get_map_stat(table).ms_entries, 6u);

  EXPECT_EQ(txn.put_multiple_samelength(table, "k3", data + 4, 3, mdbx::insert_unique), 3u)
      << "MDBX_NOOVERWRITE inserts a whole batch for a brand-new key";

  EXPECT_THROW((void)txn.put_multiple_samelength(table, "k3", data + 4, 3, mdbx::insert_unique),
               mdbx::key_exists)
      << "MDBX_NOOVERWRITE refuses a batch for an already-existing key";
  txn.abort();
  env.close();
  mdbx::env::remove("test-put-flags");
}

TEST(ut_put_flags, put_multiple_samelength_strict_failure) {
  auto env = make_env("test-put-flags");
  auto txn = env.start_write();
  auto table = txn.create_map("table", mdbx::key_mode::usual, mdbx::value_mode::multi_samelength);

  const uint32_t data[4] = {21, 22, 23, 24};
  EXPECT_EQ(txn.put_multiple_samelength(table, "k1", data, 4, mdbx::insert_unique), 4u);

  EXPECT_THROW((void)txn.put_multiple_samelength(table, "k1", data, 4, mdbx::insert_unique), mdbx::key_exists);
  txn.abort();
  env.close();
  mdbx::env::remove("test-put-flags");
}