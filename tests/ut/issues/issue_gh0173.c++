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
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Test coverage for the deferred-shrink scenarios of issue #154 (see #173).
//
// On Windows a shrink requires un-mapping the datafile, which is impossible
// while an in-process read transaction holds `remap_lock` SHARED (srw/
// `txn_ro_start()`). The fix in PR #154/#156 made such a shrink *defer* instead
// of deadlocking or poisoning the environment:
//   - dxb_sync_locked() writes the meta with the reduced logical
//     `geometry.now`, then calls dxb_resize(..., implicit_shrink);
//   - dxb_resize() cannot take the exclusive remap-lock and returns
//     MDBX_EPERM for an implicit shrink, which dxb_sync_locked() tolerates
//     (the commit still succeeds) and re-syncs `env->geo_in_bytes.now` to the
//     meta;
//   - because the shrink predicate is based on the *real* file size, a later
//     commit re-attempts the shrink once the reader is gone.
//
// The observable signature of the deferral is a transient "logical < physical"
// state: mdbx_env_info_ex().mi_geo.current (from meta.geometry.now) is smaller
// than mi_dxb_fsize (the real file size). This file pins that gap, data
// readability during the gap, the gap's durability across a close/reopen, and
// its convergence on a later commit. The explicit-resize size invariants
// (scenario C of #173) are hardened directly in issue_gh0154.c++.
//
// Portability: on POSIX read transactions never take `remap_lock` and resizes
// go through a blocking fastmutex, so there is nothing to defer and no
// "logical < physical" window — see gh0154 for the cross-platform sanity
// (returns, no hang, no poison). Windows-only assertions are compiled out and
// the tests are skipped elsewhere. Wine is NOT a valid substitute (see
// docs/engineering/debugging-methodology.md §2.13); these tests are validated
// on the real Windows CI cells.

#if defined(_WIN32) || defined(_WIN64)
namespace {

const char *const kTestPath = "issue173.mdbx";

struct reader_ctx {
  MDBX_env *env;
  MDBX_dbi dbi;
  MDBX_txn *txn;
  std::atomic<bool> started;
  std::atomic<bool> stop;
  std::atomic<int> rc;
  std::atomic<int> reads_ok; /* successful reads while pinned */
};

void reader_thread(reader_ctx *ctx) {
  ctx->txn = nullptr;
  ctx->rc.store(mdbx_txn_begin(ctx->env, nullptr, MDBX_TXN_RDONLY, &ctx->txn), std::memory_order_release);
  ctx->started.store(true, std::memory_order_release);
  if (ctx->rc.load(std::memory_order_acquire) != MDBX_SUCCESS)
    return;

  /* Keep the transaction open — on Windows this holds remap_lock shared for the
   * whole loop — and read through the pinned snapshot repeatedly. */
  static const char probe[] = "k000001";
  while (!ctx->stop.load(std::memory_order_acquire)) {
    MDBX_val key = {const_cast<char *>(probe), sizeof probe - 1};
    MDBX_val val;
    const int rc = mdbx_get(ctx->txn, ctx->dbi, &key, &val);
    if (rc == MDBX_SUCCESS || rc == MDBX_NOTFOUND)
      ctx->reads_ok.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  const int rc = mdbx_txn_abort(ctx->txn);
  if (rc != MDBX_SUCCESS)
    ctx->rc.store(rc, std::memory_order_release);
  ctx->txn = nullptr;
}

/* RAII wrapper for the reader thread: a fatal gtest assertion inside the test
 * body must never leave the thread running, otherwise ~std::thread() would
 * call std::terminate(). The destructor stops the loop, joins the thread and
 * reports the abort result. */
struct reader_guard {
  reader_ctx ctx;
  std::thread thr;

  reader_guard(MDBX_env *env, MDBX_dbi dbi) : ctx{env, dbi, nullptr, {false}, {false}, {0}, {0}}, thr() {}

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

  int abort_rc() const { return ctx.rc.load(std::memory_order_acquire); }
  int reads_ok() const { return ctx.reads_ok.load(std::memory_order_relaxed); }

  ~reader_guard() { stop_and_join(); }
};

void env_remove(const char *path) {
  try {
    mdbx::env::remove(mdbx::path(path));
  } catch (const mdbx::error &) {
    /* not required to exist beforehand */
  }
}

/* Dynamic geometry with grow/shrink steps and a shrink threshold. */
bool open_env(MDBX_env **out_env, MDBX_dbi *out_dbi, const char *path = kTestPath) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return false;
  mdbx_env_set_maxdbs(env, 4);
  /* lower 256KiB, upper 64MiB, grow/shrink step 64KiB, shrink threshold 64KiB */
  mdbx_env_set_geometry(env, 256 << 10, -1, 64 << 20, 64 << 10, 64 << 10, -1);
  /* issue #170: disable the shrink-attempt throttling, this test relies on the
   * deferred shrink being applied by the very next commit after the reader is
   * gone (see issue_gh0170 for the throttling behavior itself). */
  mdbx_env_set_option(env, MDBX_opt_shrink_retry_period, 0);
  if (mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return false;
  }
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return false;
  }
  MDBX_dbi dbi = 0;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) != MDBX_SUCCESS || mdbx_txn_commit(txn) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return false;
  }
  *out_env = env;
  *out_dbi = dbi;
  return true;
}

bool fill(MDBX_env *env, MDBX_dbi dbi, int from, int to, char filler, size_t vsize = 512) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    return false;
  std::vector<char> val(vsize, filler);
  for (int i = from; i < to; ++i) {
    char key[24];
    std::snprintf(key, sizeof key, "k%06d", i);
    MDBX_val k = {key, std::strlen(key)}, v = {val.data(), val.size()};
    if (mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      return false;
    }
  }
  return mdbx_txn_commit(txn) == MDBX_SUCCESS;
}

bool fetch_info(MDBX_env *env, MDBX_envinfo &info) {
  std::memset(&info, 0, sizeof info);
  return mdbx_env_info_ex(env, nullptr, &info, sizeof info) == MDBX_SUCCESS;
}

} // namespace

TEST(issue_gh0173, deferred_shrink_logical_lt_physical_then_converges) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  env_remove(kTestPath);
  MDBX_env *env = nullptr;
  MDBX_dbi dbi = 0;
  ASSERT_TRUE(open_env(&env, &dbi)) << "must open the env with a shrink-capable geometry";
  /* Keep the dataset small: the deferred-shrink gap below is created by the
   * explicit grow, not by the data volume, so a few hundred records is enough
   * and keeps this test cheap on the slowest CI cells. */
  ASSERT_TRUE(fill(env, dbi, 0, 512, 'x'));

  MDBX_envinfo info;
  ASSERT_TRUE(fetch_info(env, info));
  const uint64_t physical_before = info.mi_dxb_fsize;
  ASSERT_GT(physical_before, 0u);

  /* Explicitly grow the file well past the data extent while no reader holds
   * the remap-lock: first_unallocated stays small, but the physical file becomes
   * large, so the next commit sees an implicit shrink as warranted. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, 8 << 20, -1, -1, -1, -1))
      << "explicit grow without a reader must succeed";
  ASSERT_TRUE(fetch_info(env, info));
  ASSERT_GT(info.mi_dxb_fsize, physical_before) << "the explicit grow must enlarge the physical file";
  const uint64_t physical_grown = info.mi_dxb_fsize;

  /* Open a long-lived reader BEFORE the committing put: on Windows it holds
   * remap_lock shared for its whole lifetime, so the shrink attempted by the
   * next commit must be deferred. */
  reader_guard reader(env, dbi);
  ASSERT_TRUE(reader.start()) << "reader must start";

  /* A committing put: dxb_sync_locked() sees the shrink warranted (large
   * physical file, small first_unallocated) and attempts an implicit shrink.
   * With the reader open the shrink is deferred: the commit must still
   * SUCCEED, the env must stay usable, and the logical size (mi_geo.current)
   * must now be smaller than the physical one. */
  MDBX_val k = {(void *)"alive", 5}, v = {(void *)"yes", 3};
  MDBX_txn *txn = nullptr;
  bool committed = false;
  {
    const int begin_rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
    EXPECT_EQ(MDBX_SUCCESS, begin_rc);
    if (begin_rc == MDBX_SUCCESS) {
      EXPECT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
      const int commit_rc = mdbx_txn_commit(txn);
      EXPECT_EQ(MDBX_SUCCESS, commit_rc) << "the commit must succeed while the shrink is deferred";
      committed = (commit_rc == MDBX_SUCCESS);
    }
  }

  if (committed) {
    MDBX_envinfo diag;
    ASSERT_TRUE(fetch_info(env, diag));
    EXPECT_LT(diag.mi_geo.current, diag.mi_dxb_fsize) << "deferred shrink must leave logical < physical";
    EXPECT_GE(diag.mi_dxb_fsize, physical_grown) << "the physical file must not shrink while a reader holds the lock";

    /* Data integrity during the gap (E2): everything below the logical size
     * stays readable while the physical file keeps its larger size — the
     * mapping covers the whole file, so both the pre-grow data and the new
     * key are visible to a fresh read transaction. */
    MDBX_txn *ro_txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &ro_txn));
    MDBX_val got;
    MDBX_val probe_k = {(void *)"k000001", 7};
    EXPECT_EQ(MDBX_SUCCESS, mdbx_get(ro_txn, dbi, &probe_k, &got))
        << "the pre-grow data must stay readable during the deferred-shrink gap";
    EXPECT_EQ(MDBX_SUCCESS, mdbx_get(ro_txn, dbi, &k, &got)) << "the new key must be readable during the gap";
    EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_abort(ro_txn));

    /* The pinned reader must have kept reading its own snapshot all this time. */
    EXPECT_GT(reader.reads_ok(), 0) << "the pinned reader must survive the deferred shrink";

    /* The env must stay fully usable (not poisoned). */
    txn = nullptr;
    const int begin2_rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
    EXPECT_NE(MDBX_PANIC, begin2_rc) << "env was poisoned by the deferred shrink";
    ASSERT_EQ(MDBX_SUCCESS, begin2_rc);
    EXPECT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
    EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn)) << "post-deferral commit must succeed";
  }

  reader.stop_and_join();
  EXPECT_EQ(MDBX_SUCCESS, reader.abort_rc()) << "reader abort must succeed";

  /* The deferred state is durable (E1): the meta carries the reduced logical
   * `geometry.now` while the physical file keeps its larger size, and neither
   * close nor reopen reconciles it — only a later write commit re-attempts the
   * shrink (env_sync on close skips dxb_sync_locked for a steady head meta). */
  mdbx_env_close(env);
  env = nullptr;

  MDBX_env *ro_env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&ro_env));
  mdbx_env_set_maxdbs(ro_env, 4);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(ro_env, kTestPath, MDBX_NOSUBDIR | MDBX_RDONLY, 0644))
      << "the database with a deferred shrink pending must open cleanly";
  if (committed) {
    MDBX_envinfo reopen_info;
    ASSERT_TRUE(fetch_info(ro_env, reopen_info));
    EXPECT_LT(reopen_info.mi_geo.current, reopen_info.mi_dxb_fsize)
        << "the logical < physical gap must survive a close/reopen";
  }
  mdbx_env_close(ro_env);

  /* Reopen read-write and commit once more: with no reader holding the
   * remap-lock, the shrink must be re-attempted and applied — the logical and
   * physical sizes converge and the file actually shrinks. */
  ASSERT_TRUE(open_env(&env, &dbi)) << "the read-write reopen must succeed";
  ASSERT_TRUE(fill(env, dbi, 20000, 20010, 'z')) << "commit after the reader must succeed";
  ASSERT_TRUE(fetch_info(env, info));
  EXPECT_EQ(info.mi_geo.current, info.mi_dxb_fsize) << "the deferred shrink must be applied once the reader is gone";
  EXPECT_LT(info.mi_dxb_fsize, physical_grown) << "the physical file must actually shrink";

  mdbx_env_close(env);
  env_remove(kTestPath);
}
#else
TEST(issue_gh0173, deferred_shrink_logical_lt_physical_is_windows_only) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  /* On POSIX read transactions never take remap_lock, so a shrink attempted
   * while a reader is open goes through a blocking fastmutex and is applied
   * immediately; there is no deferral and therefore no "logical < physical"
   * window. The Windows-only scenarios above are skipped here. */
  GTEST_SKIP() << "deferred-shrink scenarios are Windows-only (remap_lock)";
}
#endif /* Windows */
