#include "mdbx.h++"
#include <gtest/gtest.h>
#include <cstdio>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

TEST(issue_gh0030, all) {
  const mdbx::path testdb = "issue30";
  mdbx::env::remove(testdb);
  mdbx::env::operate_parameters op(4);
  op.options.enable_validation = true;
  mdbx::env_managed env(testdb, mdbx::env_managed::create_parameters(), op);
  unsigned raw = 0;
  mdbx_env_get_flags(env, &raw);
  std::printf("raw MDBX_VALIDATION set: %d\n", (raw & MDBX_VALIDATION) != 0);
  std::printf("get_options().enable_validation: %d\n", env.get_options().enable_validation);
  EXPECT_NE(0u, raw & MDBX_VALIDATION);
  EXPECT_TRUE(env.get_options().enable_validation);
}
