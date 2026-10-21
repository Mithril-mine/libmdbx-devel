#include "mdbx.h++"
#include <cstdio>
#include <gtest/gtest.h>

#include "../test-env.h"

TEST(issue_gh0023, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
