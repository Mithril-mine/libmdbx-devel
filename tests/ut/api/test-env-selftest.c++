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

#include "test-env.h"

#include <gtest/gtest.h>

#include <ctime>

// Self-test for the shared test-environment pieces (tests/ut/test-env.h):
// the runtime_limiter transitions and the global ci_runtime_limiter instance.
// The limiter is exercised through the for_testing constructor with an
// absolute deadline, so no sleeping and no environment manipulation is
// needed and the behavior is deterministic on every platform.

TEST(test_env_selftest, limiter_disabled_never_times_out) {
  runtime_limiter disabled(runtime_limiter::for_testing(), time_t(0));
  EXPECT_FALSE(disabled.is_timeout()) << "edge == 0 means the limiter is off";
}

TEST(test_env_selftest, limiter_past_edge_times_out) {
  runtime_limiter expired(runtime_limiter::for_testing(), time(nullptr) - 1);
  EXPECT_TRUE(expired.is_timeout()) << "a deadline in the past must be reported";
}

TEST(test_env_selftest, limiter_future_edge_does_not_time_out) {
  runtime_limiter fresh(runtime_limiter::for_testing(), time(nullptr) + 3600);
  EXPECT_FALSE(fresh.is_timeout()) << "a deadline an hour ahead must not fire";
}

TEST(test_env_selftest, default_ctor_reads_environment) {
  /* The global instance is constructed from GTEST_RUNTIME_LIMIT at static
   * init: with the variable unset (local runs) or set far enough ahead (CI
   * sets CI_CTEST_TIMEOUT-60), the check must be false at test start. */
  EXPECT_FALSE(MDBX_GTEST_IS_EXECUTION_TIMEOUT())
      << "the CI runtime limit must not be reached at the very beginning of a run";
}
