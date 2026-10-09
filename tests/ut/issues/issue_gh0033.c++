#include "mdbx.h++"
#include <gtest/gtest.h>
#include <cstdio>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

TEST(issue_gh0033, all) {
  std::vector<mdbx::cursor> empty;
  mdbx::cursor from, to;
  try {
    bool r = mdbx::cursor::distribute(from, to, empty);
    std::printf("returned %d\n", r);
    FAIL() << "distribute() did not throw";
  } catch (const std::exception &e) {
    std::printf("caught (acceptable): %s\n", e.what());
    SUCCEED();
  }
}
