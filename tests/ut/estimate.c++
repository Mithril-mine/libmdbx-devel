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
mdbx::env_managed make_env(const char *name) {
  const mdbx::path testdb = name;
  mdbx::env::remove(testdb);
  return mdbx::env_managed(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
}

std::string key_of(size_t i) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "k%02zu", i);
  return buffer;
}
} // namespace

TEST(ut_estimate, empty_map) {
  auto env = make_env("test-estimate");
  auto txn = env.start_write();
  auto map = txn.create_map("table");

  EXPECT_EQ(txn.estimate_from_first(map, mdbx::slice("k00")), 0);
  EXPECT_EQ(txn.estimate_to_last(map, mdbx::slice("k00")), 0);
  EXPECT_EQ(txn.estimate(map, mdbx::slice("k00"), mdbx::slice("k99")), 0);

  txn.commit();
  env.close();
  mdbx::env::remove("test-estimate");
}

TEST(ut_estimate, single_key_exact) {
  auto env = make_env("test-estimate");
  auto txn = env.start_write();
  auto map = txn.create_map("table");
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);

  txn.upsert(map, "k05", "v");
  txn.upsert(multi, "k05", "v1");
  txn.upsert(multi, "k05", "v2");
  txn.upsert(multi, "k05", "v3");

  EXPECT_EQ(txn.estimate(map, mdbx::slice("k05"), mdbx::slice("k05")), 1) << "single-key range is exact";
  EXPECT_EQ(txn.estimate(map, mdbx::slice("k05"), mdbx::slice("k06")), 1)
      << "upper bound above the only key counts it once";
  EXPECT_EQ(txn.estimate(map, mdbx::slice("k04"), mdbx::slice("k05")), 0)
      << "lower-bound seeks to the only key, which is also the end, so zero steps";
  EXPECT_EQ(txn.estimate(multi, mdbx::slice("k05"), mdbx::slice("k05")), 3)
      << "dupsort single-key range counts all dups";

  txn.commit();
  env.close();
  mdbx::env::remove("test-estimate");
}

TEST(ut_estimate, range_sign_and_monotonicity) {
  auto env = make_env("test-estimate");
  auto txn = env.start_write();
  auto map = txn.create_map("table");
  for (size_t i = 0; i < 100; ++i)
    txn.upsert(map, mdbx::slice(key_of(i)), mdbx::slice("value"));

  const auto whole = txn.estimate(map, mdbx::slice("k00"), mdbx::slice("k99"));
  ASSERT_GT(whole, 0);
  ASSERT_EQ(whole, 99) << "single-leaf tree estimates the exact step count";

  EXPECT_LT(txn.estimate(map, mdbx::slice("k50"), mdbx::slice("k10")), 0) << "reversed range gives negative estimate";
  EXPECT_GT(txn.estimate(map, mdbx::slice("k10"), mdbx::slice("k90")), 0);
  EXPECT_LE(txn.estimate(map, mdbx::slice("k10"), mdbx::slice("k90")), whole);

  EXPECT_EQ(txn.estimate_from_first(map, mdbx::slice("k99")), whole);
  EXPECT_EQ(txn.estimate_to_last(map, mdbx::slice("k00")), whole);

  EXPECT_EQ(txn.estimate(map, mdbx::slice("k40"), mdbx::slice("k40")), 1);

  txn.commit();
  env.close();
  mdbx::env::remove("test-estimate");
}

TEST(ut_estimate, dupsort_range_with_values) {
  auto env = make_env("test-estimate");
  auto txn = env.start_write();
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);
  txn.upsert(multi, "k05", "v1");
  txn.upsert(multi, "k05", "v2");
  txn.upsert(multi, "k05", "v3");
  txn.upsert(multi, "k07", "v5");
  txn.upsert(multi, "k07", "v6");

  const mdbx::pair across_from("k05", "v1");
  const mdbx::pair across_to("k07", "v6");
  EXPECT_GT(txn.estimate(multi, across_from, across_to), 0)
      << "pair-based estimate across keys counts the whole range";
  EXPECT_LT(txn.estimate(multi, across_to, across_from), 0)
      << "reversed pair range gives a negative estimate";

  txn.commit();
  env.close();
  mdbx::env::remove("test-estimate");
}

TEST(ut_estimate, cursor_move_estimates) {
  auto env = make_env("test-estimate");
  auto txn = env.start_write();
  auto map = txn.create_map("table");
  for (size_t i = 0; i < 100; ++i)
    txn.upsert(map, mdbx::slice(key_of(i)), mdbx::slice("value"));
  txn.commit();

  auto read = env.start_read();
  auto map_open = read.open_map("table");
  auto cursor = read.open_cursor(map_open);
  cursor.to_first();

  const auto to_last = cursor.estimate(mdbx::cursor::move_operation::last);
  EXPECT_EQ(to_last.approximate_quantity, 99) << "first..last step count on a single-leaf tree";
  EXPECT_EQ(to_last.key, mdbx::slice("k99"));

  const auto to_first = cursor.estimate(mdbx::cursor::move_operation::first);
  EXPECT_EQ(to_first.approximate_quantity, 0);

  const auto to_key = cursor.estimate(mdbx::slice("k50"));
  EXPECT_EQ(to_key.key, mdbx::slice("k50")) << "lower-bound estimation returns the found key";
  EXPECT_GT(to_key.approximate_quantity, 0);
  EXPECT_LE(to_key.approximate_quantity, 99);

  cursor.close();
  read.abort();
  env.close();
  mdbx::env::remove("test-estimate");
}