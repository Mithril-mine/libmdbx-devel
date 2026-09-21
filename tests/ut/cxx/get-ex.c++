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
} // namespace

TEST(ut_get_ex, values_count_for_dupsort) {
  auto env = make_env("test-get-ex");
  auto txn = env.start_write();
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);

  txn.upsert(multi, "k", "v1");
  txn.upsert(multi, "k", "v2");
  txn.upsert(multi, "k", "v3");
  txn.upsert(multi, "other", "x");

  {
    size_t count = 0;
    EXPECT_EQ(txn.get(multi, mdbx::slice("k"), count), mdbx::slice("v1"));
    EXPECT_EQ(count, 3u) << "get_ex reports the number of values under the key";
  }
  {
    size_t count = 0;
    EXPECT_EQ(txn.get(multi, mdbx::slice("other"), count), mdbx::slice("x"));
    EXPECT_EQ(count, 1u);
  }

  EXPECT_THROW((void)txn.get(multi, mdbx::slice("absent")), mdbx::not_found);

  txn.commit();
  env.close();
  mdbx::env::remove("test-get-ex");
}

TEST(ut_get_ex, values_count_for_single_value_table) {
  auto env = make_env("test-get-ex");
  auto txn = env.start_write();
  auto table = txn.create_map("table");
  txn.upsert(table, "k", "v");

  size_t count = 0;
  EXPECT_EQ(txn.get(table, mdbx::slice("k"), count), mdbx::slice("v"));
  EXPECT_EQ(count, 1u) << "single-value table reports one item";

  txn.commit();
  env.close();
  mdbx::env::remove("test-get-ex");
}