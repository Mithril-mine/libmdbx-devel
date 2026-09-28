#include "mdbx.h++"
#include <gtest/gtest.h>
#include <cstdio>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

TEST(issue_gh0028, all) {
  mdbx::slice s("hello");
  try {
    mdbx::slice m = s.safe_middle(SIZE_MAX - 1, 2); // from + n wraps to 0 <= size() -> passes
    std::printf("no throw: len=%zu, slice points %td bytes BEFORE the data\n", m.length(),
                (const char *)s.data() - (const char *)m.data());
    FAIL() << "safe_middle() did not throw std::out_of_range";
  } catch (const std::out_of_range &) {
    SUCCEED();
  }
}
