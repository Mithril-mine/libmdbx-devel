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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" int mdbx_probe_ctl(const char *request, char *reply, size_t reply_size);

// Windows-only behavior test for issue #170: the rate-limiting of doomed
// datafile-shrink attempts.
//
// Scenario: a long-lived reader holds the remap-lock shared, so every implicit
// shrink is deferred (issue #154, covered by issue_gh0173.c++). Without the
// throttling, EVERY commit that meets the shrink predicate would pay the
// expensive dxb_resize() path (suspending local threads and try-acquiring the
// exclusive remap-lock) just to defer again. With MDBX_opt_shrink_retry_period
// set, a repeated attempt inside the period is skipped cheaply.
//
// The two outcomes are observed through the probe-bus WATCH sites inside
// dxb_shrink_attempt_allowed() (src/lck-windows.c):
//   - dxb_shrink_attempt  — an attempt was allowed and made;
//   - dxb_shrink_throttled — an attempt was skipped by the rate-limit.
// Requires a build with MDBX_PROBES=ON and MDBX_PROBES=1 in the environment
// (see tests/ut/issues/CMakeLists.txt).

#if defined(_WIN32) || defined(_WIN64)
namespace {

const char *const kTestPath = "issue170-throttle.mdbx";

struct reader_ctx {
  MDBX_env *env;
  MDBX_dbi dbi;
  MDBX_txn *txn;
  std::atomic<bool> started;
  std::atomic<bool> stop;
  std::atomic<int> rc;
};

void reader_thread(reader_ctx *ctx) {
  ctx->txn = nullptr;
  ctx->rc.store(mdbx_txn_begin(ctx->env, nullptr, MDBX_TXN_RDONLY, &ctx->txn), std::memory_order_release);
  ctx->started.store(true, std::memory_order_release);
  if (ctx->rc.load(std::memory_order_acquire) != MDBX_SUCCESS)
    return;
  while (!ctx->stop.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const int rc = mdbx_txn_abort(ctx->txn);
  if (rc != MDBX_SUCCESS)
    ctx->rc.store(rc, std::memory_order_release);
  ctx->txn = nullptr;
}

/* RAII: a fatal assertion must never leave the thread running (std::terminate). */
struct reader_guard {
  reader_ctx ctx;
  std::thread thr;

  reader_guard(MDBX_env *env, MDBX_dbi dbi) : ctx{env, dbi, nullptr, {false}, {false}, {0}}, thr() {}
  bool start() {
    thr = std::thread(reader_thread, &ctx);
    while (!ctx.started.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return ctx.rc.load(std::memory_order_acquire) == MDBX_SUCCESS;
  }
  void stop_and_join() {
    ctx.stop.store(true, std::memory_order_release);
    if (thr.joinable())
      thr.join();
  }
  ~reader_guard() { stop_and_join(); }
};

void env_remove(const char *path) {
  try {
    mdbx::env::remove(mdbx::path(path));
  } catch (const mdbx::error &) {
    /* not required to exist beforehand */
  }
}

/* Query the `seen` counter of a probe site; returns -1 when the site is not
 * (yet) registered. Reply format:
 *   site <name> <kind> <armed> <seen> <hits> <suppressed> <value> <file>:<line> */
long probe_seen(const char *tag) {
  char request[128], reply[1024];
  std::snprintf(request, sizeof request, "query %s", tag);
  if (mdbx_probe_ctl(request, reply, sizeof reply) != 0)
    return -1;
  const char *const site = std::strstr(reply, "site ");
  if (!site)
    return -1;
  unsigned seen = 0;
  if (std::sscanf(site, "site %*s %*u %*u %u", &seen) != 1)
    return -1;
  return (long)seen;
}

bool commit_put(MDBX_env *env, MDBX_dbi dbi, const char *key) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    return false;
  MDBX_val k = {const_cast<char *>(key), std::strlen(key)}, v = {const_cast<char *>("v"), 1};
  if (mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return false;
  }
  return mdbx_txn_commit(txn) == MDBX_SUCCESS;
}

} // namespace

TEST(issue_gh0170_throttle, doomed_shrink_attempts_are_rate_limited) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  env_remove(kTestPath);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  mdbx_env_set_maxdbs(env, 4);
  /* lower 256KiB, upper 64MiB, grow/shrink step 64KiB, shrink threshold 64KiB */
  mdbx_env_set_geometry(env, 256 << 10, -1, 64 << 20, 64 << 10, 64 << 10, -1);
  /* a long retry period: the second doomed attempt inside the test must be
   * throttled, not merely unlucky with the timing */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, 60 * 65536));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kTestPath, MDBX_NOSUBDIR, 0644));
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));

  /* Make an implicit shrink warranted: the physical file is explicitly grown
   * far beyond the data extent, while first_unallocated stays small. */
  ASSERT_TRUE(commit_put(env, dbi, "seed"));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, 8 << 20, -1, -1, -1, -1));

  /* Pin the remap-lock shared: from now on every shrink attempt is doomed to
   * be deferred, exactly the situation the throttling protects against. */
  reader_guard reader(env, dbi);
  ASSERT_TRUE(reader.start()) << "reader must start";

  /* The first commit performs the (doomed) attempt: it registers/fires the
   * dxb_shrink_attempt site and records the throttle timestamp. */
  ASSERT_TRUE(commit_put(env, dbi, "attempt-1"));
  const long attempt_seen_1 = probe_seen("dxb_shrink_attempt");
  ASSERT_GE(attempt_seen_1, 1) << "the first shrink attempt must be made (probe site fired)";

  /* The second commit is inside the retry period: the attempt must be skipped
   * cheaply — the throttled site fires, the attempt counter does not move. */
  ASSERT_TRUE(commit_put(env, dbi, "attempt-2"));
  const long throttled_seen = probe_seen("dxb_shrink_throttled");
  EXPECT_GE(throttled_seen, 1) << "the second doomed attempt must be throttled";
  EXPECT_EQ(probe_seen("dxb_shrink_attempt"), attempt_seen_1)
      << "a throttled commit must not re-enter the expensive shrink path";

  /* With the throttling disabled (period 0) the attempt is made again. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, 0));
  ASSERT_TRUE(commit_put(env, dbi, "attempt-3"));
  EXPECT_GT(probe_seen("dxb_shrink_attempt"), attempt_seen_1)
      << "with the zero retry period every warranted commit attempts the shrink";

  /* Sanity: the environment stayed healthy through all of the above, and once
   * the reader is gone the shrink is applied by a later commit. */
  reader.stop_and_join();
  ASSERT_TRUE(commit_put(env, dbi, "final"));
  MDBX_envinfo info;
  std::memset(&info, 0, sizeof info);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof info));
  EXPECT_EQ(info.mi_geo.current, info.mi_dxb_fsize) << "the shrink must converge once the reader is gone";

  mdbx_env_close(env);
  env_remove(kTestPath);
}
#else
TEST(issue_gh0170_throttle, windows_only) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* The throttling/exclusivity gate is a Windows-only code path (on POSIX the
   * shrink never needs un-mapping and is never deferred); dxb_shrink_attempt_
   * allowed() is a no-op returning true there. */
  GTEST_SKIP() << "shrink-attempt throttling is Windows-only";
}
#endif /* Windows */
