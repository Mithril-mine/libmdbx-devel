#include "mdbx.h++"
#include <cstdio>
#include <gtest/gtest.h>

#include "../test-env.h"

TEST(issue_gh0025, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char payload[] = "0123456789";
  mdbx::default_buffer buf(mdbx::slice(payload), /*make_reference=*/true);
  EXPECT_TRUE(buf.is_reference());
  buf.append(nullptr, 0);
  EXPECT_TRUE(buf.is_reference());
  buf.append("X", 1); // reserve_tailroom -> silo_.resize -> reshape<false> with EXTERNAL content
  EXPECT_TRUE(buf.is_freestanding());
  EXPECT_EQ(buf.length(), sizeof(payload));
  std::printf("result: %.*s (len=%zu)\n", int(buf.length()), buf.char_ptr(), buf.length());
}
