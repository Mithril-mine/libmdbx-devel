/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

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