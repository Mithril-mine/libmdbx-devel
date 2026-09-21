#include "mdbx.h++"
#include <gtest/gtest.h>
#include <cstdio>

TEST(issue_gh0023, all) {
  mdbx::path testdb = "issue_gh0023";
  mdbx::env::remove(testdb);
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), mdbx::env::operate_parameters(4));
  auto txn = env.start_write();
  auto map = txn.create_map("t");
  auto r = txn.try_insert(map, mdbx::slice("k"), mdbx::slice("v"));
  EXPECT_TRUE(r); // value_result::operator bool()
  std::printf("inserted\n");
  r = txn.try_insert(map, mdbx::slice("k"), mdbx::slice("v"));
  EXPECT_FALSE(r); // value_result::operator bool()
  std::printf("dup not inserted\n");
  txn.commit();
  env.close();
  mdbx::env::remove(testdb);
}
