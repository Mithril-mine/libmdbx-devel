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

#include <chrono>
#include <ctime>

// Self-test for the shared test-environment pieces (tests/ut/test-env.h):
// the runtime_limiter transitions and the global ci_runtime_limiter instance.
// The limiter is exercised through the for_testing constructor (a steady-clock
// offset) and through compute_edge() (the same deadline math the default
// constructor performs on the two environment variables), so no sleeping and
// no environment manipulation is needed and the behavior is deterministic on
// every platform.

namespace {

using steady_clock = std::chrono::steady_clock;

/* How close a computed deadline must be to the expected one: the computation
 * reads the wall clock, so allow a generous slack instead of an exact match. */
const std::chrono::seconds tolerance(5);

} // namespace

TEST(test_env_selftest, limiter_disabled_never_times_out) {
  runtime_limiter disabled(runtime_limiter::for_testing(), std::chrono::seconds(0));
  EXPECT_FALSE(disabled.is_timeout()) << "a zero offset means the limiter is off";
}

TEST(test_env_selftest, limiter_past_edge_times_out) {
  runtime_limiter expired(runtime_limiter::for_testing(), std::chrono::seconds(-1));
  EXPECT_TRUE(expired.is_timeout()) << "a deadline in the past must be reported";
}

TEST(test_env_selftest, limiter_future_edge_does_not_time_out) {
  runtime_limiter fresh(runtime_limiter::for_testing(), std::chrono::seconds(3600));
  EXPECT_FALSE(fresh.is_timeout()) << "a deadline an hour ahead must not fire";
}

TEST(test_env_selftest, edge_unset_when_no_limit_configured) {
  EXPECT_EQ(runtime_limiter::compute_edge(0, 0), steady_clock::time_point())
      << "without both variables the limiter must stay disabled";
  EXPECT_EQ(runtime_limiter::compute_edge(-5, 0), steady_clock::time_point())
      << "a non-positive relative limit must not arm the limiter";
}

TEST(test_env_selftest, edge_from_relative_limit) {
  const auto before = steady_clock::now();
  const auto edge = runtime_limiter::compute_edge(60, 0);
  const auto after = steady_clock::now();
  EXPECT_GE(edge, before + std::chrono::seconds(60)) << "the edge is now + the limit";
  EXPECT_LE(edge, after + std::chrono::seconds(60)) << "the edge must not be further than the limit";
}

TEST(test_env_selftest, edge_from_utc_deadline_in_the_future) {
  const auto before = steady_clock::now();
  const auto edge = runtime_limiter::compute_edge(0, std::time(nullptr) + 300);
  const auto after = steady_clock::now();
  EXPECT_GE(edge, before + std::chrono::seconds(300) - tolerance);
  EXPECT_LE(edge, after + std::chrono::seconds(300) + tolerance);
}

TEST(test_env_selftest, edge_from_utc_deadline_in_the_past) {
  const auto edge = runtime_limiter::compute_edge(0, std::time(nullptr) - 10);
  EXPECT_NE(edge, steady_clock::time_point()) << "an expired deadline still arms the limiter";
  EXPECT_LT(edge, steady_clock::now()) << "an expired deadline must time out immediately";
}

TEST(test_env_selftest, edge_is_the_earliest_of_both_limits) {
  /* A relative limit of an hour and an absolute deadline 60 seconds away:
   * the earliest must win, so a whole parallel suite shares one deadline. */
  const auto before = steady_clock::now();
  const auto edge = runtime_limiter::compute_edge(3600, std::time(nullptr) + 60);
  const auto after = steady_clock::now();
  EXPECT_GE(edge, before + std::chrono::seconds(60) - tolerance);
  EXPECT_LE(edge, after + std::chrono::seconds(60) + tolerance);

  /* And the other way round: the relative limit is the earlier one. Capture a
   * fresh window, since compute_edge() reads steady_clock::now() itself. */
  const auto before2 = steady_clock::now();
  const auto edge2 = runtime_limiter::compute_edge(60, std::time(nullptr) + 3600);
  const auto after2 = steady_clock::now();
  EXPECT_GE(edge2, before2 + std::chrono::seconds(60));
  EXPECT_LE(edge2, after2 + std::chrono::seconds(60));
}

TEST(test_env_selftest, default_ctor_reads_environment) {
  /* The global instance is constructed from GTEST_RUNTIME_LIMIT and
   * GTEST_RUNTIME_DEADLINE at static init: with the variables unset (local
   * runs) or set far enough ahead (CI sets CI_CTEST_TIMEOUT-60), the check
   * must be false at test start. */
  EXPECT_FALSE(MDBX_GTEST_IS_EXECUTION_TIMEOUT())
      << "the CI runtime limit must not be reached at the very beginning of a run";
}
