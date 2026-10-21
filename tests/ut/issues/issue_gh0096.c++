#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/96
// mdbx_liberr2str() had no message for MDBX_BACKLOG_DEPLETED, so it returned
// NULL: mdbx_strerror() fell back to the system message table and the C++
// mdbx::error::what() returned a null pointer, which crashes a typical C++
// error handler formatting the message.

TEST(issue_gh0096, backlog_depleted_has_message) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  EXPECT_NE(nullptr, mdbx_liberr2str(MDBX_BACKLOG_DEPLETED)) << "a libmdbx-specific code must have a message";
  const char *const str = mdbx_strerror(MDBX_BACKLOG_DEPLETED);
  EXPECT_NE(nullptr, str);
  {
    ASSERT_NE(nullptr, str);
    EXPECT_NE(nullptr, strstr(str, "BACKLOG_DEPLETED")) << "the message must name the code";
  }
}