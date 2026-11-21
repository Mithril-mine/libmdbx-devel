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

#include "../env-guard.h++"
#include "mdbx.h++"
#include <gtest/gtest.h>

#include "../test-env.h++"

TEST(ut_txn_metadata, canary) {
  const env_files_guard guard({"test-txn-metadata"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
  const env_files_guard guard({"test-txn-metadata"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
  const env_files_guard guard({"test-txn-metadata"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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

TEST(ut_txn_metadata, map_info_modes_from_flags) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  using info = mdbx::map_handle::info;
  const mdbx::map_handle::state state{};

  /* declared members map to themselves */
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_DB_DEFAULTS), state).key_mode(), mdbx::key_mode::usual);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_REVERSEKEY), state).key_mode(), mdbx::key_mode::reverse);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_INTEGERKEY), state).key_mode(), mdbx::key_mode::ordinal);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_DB_DEFAULTS), state).value_mode(), mdbx::value_mode::single);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_DUPSORT), state).value_mode(), mdbx::value_mode::multi);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_DUPSORT | MDBX_REVERSEDUP), state).value_mode(), mdbx::value_mode::multi_reverse);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_DUPSORT | MDBX_DUPFIXED), state).value_mode(),
            mdbx::value_mode::multi_samelength);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_REVERSEDUP), state).value_mode(),
            mdbx::value_mode::multi_reverse_samelength);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP), state).value_mode(),
            mdbx::value_mode::multi_ordinal);
  /* the reversed-ordinal combination is accepted by the core but has no
   * dedicated member; it must not produce an undeclared enum value */
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_DUPSORT | MDBX_DUPFIXED | MDBX_INTEGERDUP | MDBX_REVERSEDUP), state).value_mode(),
            mdbx::value_mode::multi_ordinal);
  /* masked combinations which the core rejects must map to declared members
   * instead of undeclared scoped-enum values */
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_REVERSEKEY | MDBX_INTEGERKEY), state).key_mode(), mdbx::key_mode::ordinal);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_REVERSEDUP), state).value_mode(), mdbx::value_mode::single);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_INTEGERDUP), state).value_mode(), mdbx::value_mode::single);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_REVERSEDUP | MDBX_DUPFIXED | MDBX_INTEGERDUP), state).value_mode(),
            mdbx::value_mode::single);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_DUPSORT | MDBX_INTEGERDUP), state).value_mode(), mdbx::value_mode::multi);
  EXPECT_EQ(info(MDBX_db_flags_t(MDBX_DUPSORT | MDBX_REVERSEDUP | MDBX_INTEGERDUP), state).value_mode(),
            mdbx::value_mode::multi);
}

TEST(ut_txn_metadata, map_info_modes_of_created_tables) {
  const env_files_guard guard({"test-txn-metadata"});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const mdbx::path testdb = "test-txn-metadata";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::create_parameters(), mdbx::operate_parameters().set_max_maps(8));
  {
    auto txn = env.start_write();
    auto usual = txn.create_map("usual");
    EXPECT_EQ(txn.get_map_flags(usual).key_mode(), mdbx::key_mode::usual);
    EXPECT_EQ(txn.get_map_flags(usual).value_mode(), mdbx::value_mode::single);

    auto reverse = txn.create_map("reverse", mdbx::key_mode::reverse);
    EXPECT_EQ(txn.get_map_flags(reverse).key_mode(), mdbx::key_mode::reverse);

    auto ordinal = txn.create_map("ordinal", mdbx::key_mode::ordinal);
    EXPECT_EQ(txn.get_map_flags(ordinal).key_mode(), mdbx::key_mode::ordinal);

    auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);
    EXPECT_EQ(txn.get_map_flags(multi).value_mode(), mdbx::value_mode::multi);

    auto multi_ordinal = txn.create_map("multi-ordinal", mdbx::key_mode::usual, mdbx::value_mode::multi_ordinal);
    EXPECT_EQ(txn.get_map_flags(multi_ordinal).value_mode(), mdbx::value_mode::multi_ordinal);

    auto multi_reverse_samelength =
        txn.create_map("multi-reverse-samelength", mdbx::key_mode::usual, mdbx::value_mode::multi_reverse_samelength);
    EXPECT_EQ(txn.get_map_flags(multi_reverse_samelength).value_mode(), mdbx::value_mode::multi_reverse_samelength);
    txn.commit();
  }
  env.close();
  mdbx::env::remove(testdb);
}