#include "mdbx.h++"
#include <cstdio>
#include <gtest/gtest.h>

#include "../test-env.h"

#include <utility>

TEST(issue_gh0024, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char payload[] = "external-payload";
  mdbx::default_buffer ref(mdbx::slice(payload), /*make_reference=*/true);
  mdbx::default_buffer dst;
  dst = std::move(ref); // assign(buffer&&) -> src.data() non-const -> assert(is_freestanding())
  EXPECT_TRUE(dst.is_reference());
  EXPECT_EQ(dst.length(), sizeof(payload) - 1);
  std::printf("moved: %zu bytes\n", dst.length());
}
