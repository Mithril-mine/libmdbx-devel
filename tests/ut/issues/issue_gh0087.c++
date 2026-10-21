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
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/87
// mdbx_jsonInteger_from_key() decoded the key of 0 as INT64_MAX: the encoder
// maps 0 to the bias (0x8000000000000000), but the decoder split positive from
// negative on `key > bias`, so `key == bias` fell into the negative branch,
// the exponent wrapped to all-ones and the "too large" branch fired.

TEST(issue_gh0087, zero_roundtrips) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const int64_t vals[] = {0, 1, -1, 42, -42, INT64_MAX, INT64_MIN};
  for (const int64_t expected : vals) {
    const uint64_t key = mdbx_key_from_jsonInteger(expected);
    MDBX_val kv;
    kv.iov_base = const_cast<uint64_t *>(&key);
    kv.iov_len = sizeof(key);
    const int64_t actual = mdbx_jsonInteger_from_key(kv);
    EXPECT_EQ(expected, actual) << "jsonInteger " << expected << " round-trip";
  }
}
