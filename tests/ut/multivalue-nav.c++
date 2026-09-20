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

void seed_multi(mdbx::txn_managed &txn, mdbx::map_handle map) {
  for (int key = 0; key < 3; ++key) {
    char k[8];
    snprintf(k, sizeof(k), "k%d", key);
    const int nvals = 3 - key; /* k0 -> 3 values, k1 -> 2 values, k2 -> 1 value */
    for (int v = 0; v < nvals; ++v) {
      char val[8];
      snprintf(val, sizeof(val), "v%d", v);
      txn.upsert(map, mdbx::slice(k, strlen(k)), mdbx::slice(val, strlen(val)));
    }
  }
}
} // namespace

TEST(ut_multivalue_nav, find_and_bounds) {
  auto env = make_env("test-multivalue-nav");
  auto txn = env.start_write();
  auto map = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);
  seed_multi(txn, map);

  auto cur = txn.open_cursor(map);

  auto found = cur.find_multivalue("k0", "v1");
  ASSERT_TRUE(found.done);
  EXPECT_EQ(found.key, mdbx::slice("k0"));
  EXPECT_EQ(found.value, mdbx::slice("v1"));

  EXPECT_FALSE(cur.find_multivalue("k0", "v9", false).done) << "absent pair must report 'not done'";
  EXPECT_THROW((void)cur.find_multivalue("k0", "v9", true), mdbx::not_found);

  EXPECT_EQ(cur.lower_bound_multivalue("k1", "v1").value, mdbx::slice("v1")) << "exact value is the lower bound";
  EXPECT_EQ(cur.lower_bound_multivalue("k1", "v0x").value, mdbx::slice("v1")) << "lower bound of absent value";
  EXPECT_FALSE(cur.lower_bound_multivalue("k1", "v1x", false).done) << "nothing above the last value of k1";
  EXPECT_EQ(cur.upper_bound_multivalue("k1", "v0").value, mdbx::slice("v1")) << "upper bound is strictly greater";
  EXPECT_FALSE(cur.upper_bound_multivalue("k1", "v1", false).done) << "no value above the last one of k1";

  cur.close();
  txn.commit();
  env.close();
  mdbx::env::remove("test-multivalue-nav");
}

TEST(ut_multivalue_nav, walk_multi_values) {
  auto env = make_env("test-multivalue-nav");
  auto txn = env.start_write();
  auto map = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);
  seed_multi(txn, map);

  auto cur = txn.open_cursor(map);
  cur.find_multivalue("k0", "v0");

  EXPECT_TRUE(cur.on_first_multival());
  EXPECT_FALSE(cur.on_last_multival());
  EXPECT_EQ(cur.count_multivalue(), 3u);

  EXPECT_EQ(cur.to_current_next_multi().value, mdbx::slice("v1"));
  EXPECT_EQ(cur.to_current_next_multi().value, mdbx::slice("v2"));
  EXPECT_TRUE(cur.on_last_multival());
  EXPECT_THROW((void)cur.to_current_next_multi(true), mdbx::not_found);

  EXPECT_EQ(cur.to_current_prev_multi().value, mdbx::slice("v1"));
  EXPECT_EQ(cur.to_current_first_multi().value, mdbx::slice("v0"));
  EXPECT_EQ(cur.to_current_last_multi().value, mdbx::slice("v2"));

  EXPECT_EQ(cur.to_next_first_multi().key, mdbx::slice("k1"));
  EXPECT_EQ(cur.count_multivalue(), 2u);

  cur.close();
  txn.commit();
  env.close();
  mdbx::env::remove("test-multivalue-nav");
}

TEST(ut_multivalue_nav, erase_pair_and_whole) {
  auto env = make_env("test-multivalue-nav");
  auto txn = env.start_write();
  auto map = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);
  seed_multi(txn, map);

  EXPECT_TRUE(txn.erase(map, "k1", "v1")) << "erase particular multi-value";
  EXPECT_FALSE(txn.erase(map, "k1", "v1")) << "already removed pair is not found";
  EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("v0")) << "sibling value stays intact";

  EXPECT_TRUE(txn.erase(map, "k2")) << "erase whole multi-value";
  EXPECT_THROW((void)txn.get(map, "k2"), mdbx::not_found);

  txn.commit();
  env.close();
  mdbx::env::remove("test-multivalue-nav");
}