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

TEST(ut_txn_dataops, insert_and_try_insert) {
  auto env = make_env("test-txn-dataops");
  auto txn = env.start_write();
  auto map = txn.create_map("table");

  txn.insert(map, "k1", "v1");
  EXPECT_THROW(txn.insert(map, "k1", "v2"), mdbx::key_exists);

  const auto ok = txn.try_insert(map, "k2", "v2");
  EXPECT_TRUE(ok.done);
  EXPECT_EQ(txn.get(map, "k2"), mdbx::slice("v2"));

  const auto dup = txn.try_insert(map, "k1", "vX");
  EXPECT_FALSE(dup.done);
  EXPECT_EQ(dup.value, mdbx::slice("v1")) << "returns the present value";

  txn.commit();
  env.close();
  mdbx::env::remove("test-txn-dataops");
}

TEST(ut_txn_dataops, upsert_update_erase) {
  auto env = make_env("test-txn-dataops");
  auto txn = env.start_write();
  auto map = txn.create_map("table");

  txn.upsert(map, "k1", "v1");
  txn.upsert(map, "k1", "v1b");
  EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("v1b"));

  EXPECT_THROW(txn.update(map, "absent", "x"), mdbx::not_found);
  EXPECT_TRUE(txn.try_update(map, "k1", "v1c"));
  EXPECT_FALSE(txn.try_update(map, "absent", "x"));
  EXPECT_EQ(txn.get(map, "k1"), mdbx::slice("v1c"));

  EXPECT_TRUE(txn.erase(map, "k1"));
  EXPECT_FALSE(txn.erase(map, "k1"));
  EXPECT_THROW(txn.get(map, "k1"), mdbx::not_found);

  txn.commit();
  env.close();
  mdbx::env::remove("test-txn-dataops");
}

TEST(ut_txn_dataops, extract_and_append) {
  auto env = make_env("test-txn-dataops");
  auto txn = env.start_write();
  auto map = txn.create_map("table");

  txn.insert(map, "k1", "v1");
  auto extracted = txn.extract<mdbx::buffer<>>(map, mdbx::slice("k1"));
  EXPECT_TRUE(extracted == mdbx::slice("v1"));
  EXPECT_THROW(txn.get(map, "k1"), mdbx::not_found);
  EXPECT_THROW((void)txn.extract<mdbx::buffer<>>(map, mdbx::slice("k1")), mdbx::not_found);

  txn.append(map, "a", "1");
  txn.append(map, "b", "2");
  txn.append(map, "c", "3");
  EXPECT_EQ(txn.get(map, "a"), mdbx::slice("1"));
  EXPECT_EQ(txn.get(map, "c"), mdbx::slice("3"));

  txn.commit();
  env.close();
  mdbx::env::remove("test-txn-dataops");
}

TEST(ut_txn_dataops, get_default_and_try_start_write) {
  auto env = make_env("test-txn-dataops");

  auto writer = env.try_start_write();
  ASSERT_TRUE(writer);
  EXPECT_THROW(env.try_start_write(), mdbx::something_busy) << "another writer must not be able to start";
  writer.abort();

  writer = env.try_start_write();
  ASSERT_TRUE(writer);
  auto map = writer.create_map("table");
  writer.upsert(map, "k", "v");
  writer.commit();

  auto read = env.start_read();
  EXPECT_EQ(read.get(map, "k"), mdbx::slice("v"));
  read.abort();

  env.close();
  mdbx::env::remove("test-txn-dataops");
}

TEST(ut_txn_dataops, amend_read_to_write) {
  auto env = make_env("test-txn-dataops");
  {
    auto w = env.start_write();
    w.create_map("table");
    w.commit();
  }
  auto txn = env.start_read();
  auto map = txn.open_map("table");
  ASSERT_TRUE(txn.amend());
  txn.upsert(map, "k", "v");
  txn.commit();

  auto read = env.start_read();
  EXPECT_EQ(read.get(map, "k"), mdbx::slice("v"));
  read.abort();

  env.close();
  mdbx::env::remove("test-txn-dataops");
}