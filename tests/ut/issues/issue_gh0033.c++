#include "mdbx.h++"
#include <cstdio>
#include <gtest/gtest.h>

#include "../test-env.h"

#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

TEST(issue_gh0033, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
