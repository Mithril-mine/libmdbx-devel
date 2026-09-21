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

TEST(ut_txn_metadata, canary) {
  const mdbx::path testdb = "test-txn-metadata";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    mdbx::txn::canary canary{0, 0, 0, 0};
    canary.x = 0x1122334455667788ull;
    canary.y = 0xaabbccddeeff0011ull;
    canary.z = 42;
    txn.put_canary(canary);
    txn.commit();
  }
  {
    auto txn = env.start_read();
    const mdbx::txn::canary canary = txn.get_canary();
    EXPECT_EQ(canary.x, 0x1122334455667788ull);
    EXPECT_EQ(canary.y, 0xaabbccddeeff0011ull);
    EXPECT_EQ(canary.z, 42u);
    EXPECT_GT(canary.v, 0u) << "v is set to the transaction number";
    txn.abort();
  }
  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_txn_metadata, sequence) {
  const mdbx::path testdb = "test-txn-metadata";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto map = txn.create_map("table");
    EXPECT_EQ(txn.sequence(map), 0u);
    EXPECT_EQ(txn.sequence(map, 10), 0u) << "returns the value before the change";
    EXPECT_EQ(txn.sequence(map), 10u);
    EXPECT_EQ(txn.sequence(map, 5), 10u) << "returns the value before the change";
    EXPECT_EQ(txn.sequence(map), 15u);
    txn.commit();
  }
  {
    auto txn = env.start_read();
    auto map = txn.open_map("table");
    EXPECT_EQ(txn.sequence(map), 15u);
    txn.abort();
  }
  env.close();
  mdbx::env::remove(testdb);
}

TEST(ut_txn_metadata, compare_keys_and_values) {
  const mdbx::path testdb = "test-txn-metadata";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto usual = txn.create_map("usual");
    auto multi_ordinal = txn.create_map("multi-ordinal", mdbx::key_mode::usual, mdbx::value_mode::multi_ordinal);
    auto reverse = txn.create_map("reverse", mdbx::key_mode::reverse);

    EXPECT_LT(txn.compare_keys(usual, mdbx::slice("a"), mdbx::slice("b")), 0);
    EXPECT_GT(txn.compare_keys(usual, mdbx::slice("b"), mdbx::slice("a")), 0);
    EXPECT_EQ(txn.compare_keys(usual, mdbx::slice("a"), mdbx::slice("a")), 0);

    const uint32_t k1 = mdbx::key_from_int32(1);
    const uint32_t k2 = mdbx::key_from_int32(2);
    EXPECT_LT(txn.compare_values(multi_ordinal, mdbx::slice(&k1, sizeof(k1)), mdbx::slice(&k2, sizeof(k2))), 0);
    EXPECT_GT(txn.compare_values(multi_ordinal, mdbx::slice(&k2, sizeof(k2)), mdbx::slice(&k1, sizeof(k1))), 0);
    EXPECT_EQ(txn.compare_values(multi_ordinal, mdbx::slice(&k1, sizeof(k1)), mdbx::slice(&k1, sizeof(k1))), 0);

    EXPECT_GT(txn.compare_keys(reverse, mdbx::slice("b"), mdbx::slice("a")), 0)
        << "reverse mode compares keys in reversed order";
    txn.commit();
  }
  env.close();
  mdbx::env::remove(testdb);
}