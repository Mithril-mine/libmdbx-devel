#include "mdbx.h++"
#include <gtest/gtest.h>
#include <cstdio>

TEST(issue_gh0025, all) {
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
