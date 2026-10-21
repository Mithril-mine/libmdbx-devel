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
