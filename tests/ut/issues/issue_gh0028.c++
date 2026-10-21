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

#include "mdbx.h++"
#include <cstdio>
#include <gtest/gtest.h>

#include "../test-env.h"

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

TEST(issue_gh0028, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
