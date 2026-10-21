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

TEST(issue_gh0030, all) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
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
