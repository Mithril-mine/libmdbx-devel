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

#include <cstring>

namespace {
mdbx::env_managed make_env(const char *name) {
  const mdbx::path testdb = name;
  mdbx::env::remove(testdb);
  return mdbx::env_managed(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
}
} // namespace

TEST(ut_put_flags, reserve_variants) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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

TEST(ut_put_flags, cursor_reserve_variants) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  auto env = make_env("test-put-flags");
  auto txn = env.start_write();
  auto map = txn.create_map("table");
  auto cursor = txn.open_cursor(map);

  {
    auto reservation = cursor.insert_reserve("k1", 4);
    ASSERT_EQ(reservation.size(), 4u);
    memcpy(reservation.byte_ptr(), "abcd", 4);
    EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("abcd"));
  }

  {
    const auto duplicate = cursor.try_insert_reserve("k1", 8);
    EXPECT_FALSE(duplicate.done);
    EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("abcd")) << "existing value must survive a failed reserve";
  }

  {
    auto reservation = cursor.upsert_reserve("k2", 7);
    ASSERT_EQ(reservation.size(), 7u);
    memcpy(reservation.byte_ptr(), "abcdefg", 7);
    EXPECT_EQ(txn.get(map, "k2"), mdbx::slice("abcdefg"));
  }

  {
    ASSERT_TRUE(cursor.seek("k1"));
    auto reservation = cursor.update_reserve("k1", 3);
    ASSERT_EQ(reservation.size(), 3u);
    memcpy(reservation.byte_ptr(), "XYZ", 3);
    EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("XYZ"));
  }

  {
    ASSERT_TRUE(cursor.seek("k2"));
    auto updated = cursor.try_update_reserve("k2", 2);
    ASSERT_TRUE(updated.done);
    ASSERT_EQ(updated.value.size(), 2u);
    memcpy(updated.value.byte_ptr(), "QR", 2);
    EXPECT_EQ(txn.get(map, "k2"), mdbx::slice("QR"));
  }

  {
    ASSERT_TRUE(cursor.seek("k1"));
    auto reservation = cursor.reserve_current(5);
    ASSERT_EQ(reservation.size(), 5u);
    memcpy(reservation.byte_ptr(), "12345", 5);
    EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("12345"));
  }

  EXPECT_THROW((void)cursor.upsert_reserve("k3", size_t(MDBX_MAXDATASIZE) + 1), std::length_error)
      << "an oversize reservation must be rejected as before PR #147";

  txn.commit();
  env.close();
  mdbx::env::remove("test-put-flags");
}

TEST(ut_put_flags, reserve_forbidden_on_dupsort) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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

  EXPECT_THROW((void)txn.put_multiple_samelength(table, "k3", data + 4, 3, mdbx::insert_unique), mdbx::key_exists)
      << "MDBX_NOOVERWRITE refuses a batch for an already-existing key";
  txn.abort();
  env.close();
  mdbx::env::remove("test-put-flags");
}

TEST(ut_put_flags, replace_and_get_equal_or_great) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  auto env = make_env("test-put-flags");
  auto txn = env.start_write();
  auto map = txn.create_map("table");

  txn.insert(map, "k1", "old-value");
  txn.insert(map, "k3", "three");

  {
    const auto old = txn.replace<mdbx::buffer<>>(map, "k1", "new-value");
    EXPECT_EQ(old, mdbx::slice("old-value"));
    EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("new-value"));
  }

  {
    mdbx::slice reservation;
    const auto prev = txn.replace_reserve<mdbx::buffer<>>(map, "k1", 3, reservation);
    EXPECT_EQ(prev, mdbx::slice("new-value"));
    ASSERT_EQ(reservation.size(), 3u);
    memcpy(reservation.byte_ptr(), "XYZ", 3);
    EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("XYZ"));
  }

  {
    const auto exact = txn.get_equal_or_great(map, "k1");
    EXPECT_TRUE(exact.done);
    EXPECT_EQ(exact.key, mdbx::slice("k1"));
    EXPECT_EQ(exact.value, mdbx::slice("XYZ"));

    const auto nearest = txn.get_equal_or_great(map, "k0");
    EXPECT_FALSE(nearest.done) << "k0 is absent, lower bound lands on the next key";
    EXPECT_EQ(nearest.key, mdbx::slice("k1"));

    EXPECT_THROW((void)txn.get_equal_or_great(map, "k9"), mdbx::not_found)
        << "no equal-or-greater key after the last one";
  }

  txn.commit();
  env.close();
  mdbx::env::remove("test-put-flags");
}

TEST(ut_put_flags, put_multiple_samelength_iovlen_on_keyexist) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  auto env = make_env("test-put-flags");
  auto txn = env.start_write();
  auto table = txn.create_map("table", mdbx::key_mode::usual, mdbx::value_mode::multi_samelength);

  const uint32_t data[4] = {21, 22, 23, 24};
  const uint32_t extra[3] = {25, 26, 27};
  EXPECT_EQ(txn.put_multiple_samelength(table, "k1", data, 4, mdbx::insert_unique), 4u);

  EXPECT_EQ(txn.put_multiple_samelength(table, "k1", extra, 3, mdbx::insert_unique, /*allow_partial=*/true), 0u)
      << "MDBX_NOOVERWRITE|MDBX_MULTIPLE on an existing key must report ZERO written items, "
         "not the input count";
  EXPECT_EQ(txn.get_map_stat(table).ms_entries, 4u) << "no item may be added to the existing key";

  txn.commit();
  env.close();
  mdbx::env::remove("test-put-flags");
}

TEST(ut_put_flags, put_multiple_samelength_strict_failure) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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