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

size_t multivalue_count(mdbx::txn &txn, mdbx::map_handle map) {
  auto cursor = txn.open_cursor(map);
  cursor.to_first();
  return cursor.count_multivalue();
}
} // namespace

TEST(ut_put_matrix, single_value_table) {
  auto env = make_env("test-put-matrix");
  auto txn = env.start_write();
  auto table = txn.create_map("table");

  /* NOOVERWRITE: insert of a fresh key */
  txn.insert(table, "a", "1");
  EXPECT_EQ(txn.get(table, "a"), mdbx::slice("1"));

  /* NOOVERWRITE on an existing key -> MDBX_KEYEXIST */
  EXPECT_THROW((void)txn.insert(table, "a", "2"), mdbx::key_exists);
  EXPECT_EQ(txn.get(table, "a"), mdbx::slice("1")) << "the old value survives";

  /* UPSERT: update of an existing key */
  txn.upsert(table, "a", "3");
  EXPECT_EQ(txn.get(table, "a"), mdbx::slice("3"));

  /* UPSERT: insert of a fresh key */
  txn.upsert(table, "b", "4");
  EXPECT_EQ(txn.get(table, "b"), mdbx::slice("4"));

  /* CURRENT: update of an existing key */
  {
    mdbx::slice next("5");
    EXPECT_EQ(txn.put(table, "a", &next, MDBX_CURRENT), MDBX_SUCCESS);
    EXPECT_EQ(txn.get(table, "a"), mdbx::slice("5"));
  }

  /* CURRENT on an absent key -> MDBX_NOTFOUND */
  {
    mdbx::slice probe("x");
    EXPECT_EQ(txn.put(table, "absent", &probe, MDBX_CURRENT), MDBX_NOTFOUND);
  }

  txn.commit();
  env.close();
  mdbx::env::remove("test-put-matrix");
}

TEST(ut_put_matrix, dupsort_table) {
  auto env = make_env("test-put-matrix");
  auto txn = env.start_write();
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);

  /* UPSERT appends distinct values */
  txn.upsert(multi, "k", "v1");
  txn.upsert(multi, "k", "v2");
  EXPECT_EQ(multivalue_count(txn, multi), 2u);

  /* UPSERT|ALLDUPS replaces the whole value set with one item */
  {
    mdbx::slice repl("only");
    EXPECT_EQ(txn.put(multi, "k", &repl, MDBX_UPSERT | MDBX_ALLDUPS), MDBX_SUCCESS);
    EXPECT_EQ(multivalue_count(txn, multi), 1u);
  }

  /* CURRENT on multiple values replaces the whole set with one value
   * (mdbx.h matrix line 142 says EMULTIVAL, but current code overwrites;
   * EMULTIVAL is returned for CURRENT|NOOVERWRITE below). */
  txn.upsert(multi, "k2", "x");
  txn.upsert(multi, "k2", "y");
  {
    mdbx::slice probe("z");
    EXPECT_EQ(txn.put(multi, "k2", &probe, MDBX_CURRENT), MDBX_SUCCESS);
    EXPECT_EQ(multivalue_count(txn, multi), 1u);
  }

  /* CURRENT|NOOVERWRITE on multiple values -> MDBX_EMULTIVAL */
  txn.upsert(multi, "k3", "p");
  txn.upsert(multi, "k3", "q");
  {
    mdbx::slice probe("r");
    EXPECT_EQ(txn.put(multi, "k3", &probe, MDBX_CURRENT | MDBX_NOOVERWRITE), MDBX_EMULTIVAL);
  }

  txn.commit();
  env.close();
  mdbx::env::remove("test-put-matrix");
}

TEST(ut_put_matrix, del_matrix) {
  auto env = make_env("test-put-matrix");
  auto txn = env.start_write();
  auto multi = txn.create_map("multi", mdbx::key_mode::usual, mdbx::value_mode::multi);

  txn.upsert(multi, "k", "v1");
  txn.upsert(multi, "k", "v2");
  txn.upsert(multi, "k", "v3");

  /* delete by key when the key is absent -> false */
  EXPECT_FALSE(txn.erase(multi, "absent"));

  /* delete of a missing key/value pair -> false */
  EXPECT_FALSE(txn.erase(multi, "k", "nope"));
  EXPECT_EQ(multivalue_count(txn, multi), 3u);

  /* NODUPDATA-style: delete a single value */
  EXPECT_TRUE(txn.erase(multi, "k", "v2"));
  EXPECT_EQ(multivalue_count(txn, multi), 2u);

  /* ALLDUPS-style: delete the whole key */
  EXPECT_TRUE(txn.erase(multi, "k"));
  EXPECT_THROW((void)txn.get(multi, "k"), mdbx::not_found);

  txn.commit();
  env.close();
  mdbx::env::remove("test-put-matrix");
}