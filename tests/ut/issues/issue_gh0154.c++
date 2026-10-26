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

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression coverage for the Windows `remap_lock` deadlock and its fix
// (issue #154). Runs on every platform, but only the Windows CI cells exercise
// the deferral path itself — see "Portability" below.
//
// The bug: a long-lived read-only transaction holds `remap_lock` SHARED for its
// whole lifetime (txn_ro_start(), Windows only), while dxb_resize() acquired it
// EXCLUSIVELY and unconditionally. A writer resizing the file therefore blocked
// on the reader, and the reader blocked on its stop signal — gh0083 hung for
// 600 s on the win-mingw-64 and win-cxx-v142 cells. The fix takes the lock
// SHARED for a plain in-place grow (NtExtendSection, fits the mapping limit) and
// TRY-acquires it exclusively (a few bounded attempts) for a shrink or a remap,
// deferring — never waiting — while a reader holds it shared.
//
// Invariants pinned here:
//   1. a resize while a reader is open must RETURN, never hang — ctest enforces
//      a hard timeout, so a hang fails the run as a deadlock regression (this is
//      the part that reproduces #154 on the Windows cells);
//   2. a deferred resize must NOT poison the environment: an implicit shrink is
//      deferred with MDBX_EPERM which dxb_sync_locked() tolerates (the commit
//      still succeeds and the shrink is re-attempted later), while an explicit
//      resize / a grow that needs a remap returns MDBX_UNABLE_EXTEND_MAPSIZE,
//      which the caller propagates without touching env->flags;
//   3. the pinned reader keeps reading its snapshot, and the env stays writable
//      before and after the reader releases.
//
// Portability: on POSIX read transactions never take remap_lock and resizes go
// through a blocking fastmutex, so neither the deadlock nor the deferral can
// occur there — on Linux/macOS this test is a sanity check of the grow/resize
// paths with a concurrent reader. Note that the F1 poisoning itself is only
// reachable via the implicit-shrink path (dxb_sync_locked), which is not
// deterministically triggerable from a test; invariant 2 is therefore guarded
// primarily by the EPERM/UNABLE_EXTEND_MAPSIZE split in
// windows_resize_decide_without_exclusive() and by invariant 1 on the Windows
// cells. See docs/engineering/debugging-methodology.md §2.13 — Wine is NOT a
// substitute for the Windows CI cells.

namespace {

const char *const kTestPath = "issue154.mdbx";

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

void env_remove(const char *path) {
  try {
    mdbx::env::remove(mdbx::path(path));
  } catch (const mdbx::error &) {
    /* not required to exist beforehand */
  }
}

/* Dynamic geometry with grow/shrink steps and a shrink threshold, so commits may
 * shrink and (on Windows) readers take remap_lock shared — the #154 condition. */
bool open_env(MDBX_env **out_env, MDBX_dbi *out_dbi, const char *path = kTestPath) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return false;
  mdbx_env_set_maxdbs(env, 4);
  /* lower 256KiB, upper 64MiB, grow/shrink step 64KiB, shrink threshold 64KiB */
  mdbx_env_set_geometry(env, 256 << 10, -1, 64 << 20, 64 << 10, 64 << 10, -1);
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

bool fill(MDBX_env *env, MDBX_dbi dbi, int from, int to, char filler) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    return false;
  for (int i = from; i < to; ++i) {
    char key[24], val[512];
    std::snprintf(key, sizeof key, "k%06d", i);
    std::memset(val, filler, sizeof val);
    MDBX_val k = {key, std::strlen(key)}, v = {val, sizeof val};
    if (mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) != MDBX_SUCCESS) {
      mdbx_txn_abort(txn);
      return false;
    }
  }
  return mdbx_txn_commit(txn) == MDBX_SUCCESS;
}

} // namespace

TEST(issue_gh0154, resize_with_open_reader_neither_hangs_nor_poisons_env) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  env_remove(kTestPath);
  MDBX_env *env = nullptr;
  MDBX_dbi dbi = 0;
  ASSERT_TRUE(open_env(&env, &dbi)) << "must open the env with a shrink-capable geometry";
  ASSERT_TRUE(fill(env, dbi, 0, 2000, 'x'));

  /* A long-lived reader: on Windows it holds remap_lock shared for its whole
   * lifetime — exactly what blocked the writer's exclusive remap before #154. */
  reader_ctx reader = {env, dbi, nullptr, {false}, {false}, {0}, {0}};
  std::thread thr(reader_thread, &reader);
  while (!reader.started.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(MDBX_SUCCESS, reader.rc.load(std::memory_order_acquire)) << "reader must start";

  /* Writes while the reader is open must keep succeeding: this is the grow path
   * (shared remap-lock, in-place section extension) that gh0083 deadlocked on. */
  ASSERT_TRUE(fill(env, dbi, 2000, 4000, 'y')) << "growth with an open reader must not deadlock (issue #154)";

  /* Force a resize that needs the mapping unmapped: an explicit geometry change
   * shrinking `upper`. On Windows with the reader open the exclusive remap-lock
   * cannot be taken, so the resize is deferred — it must RETURN
   * (SUCCESS / UNABLE_EXTEND_MAPSIZE / EPERM), never hang, and never poison the
   * environment. */
  MDBX_envinfo info;
  std::memset(&info, 0, sizeof info);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof info));
  const intptr_t cur_upper = (intptr_t)info.mi_geo.upper;
  const intptr_t smaller = (intptr_t)info.mi_geo.current + (intptr_t)(512 << 10);
  const int geo_rc = mdbx_env_set_geometry(env, -1, -1, (smaller < cur_upper) ? smaller : cur_upper, -1, -1, -1);
  EXPECT_TRUE(geo_rc == MDBX_SUCCESS || geo_rc == MDBX_UNABLE_EXTEND_MAPSIZE || geo_rc == MDBX_EPERM)
      << "an explicit resize with an open reader must defer, not fail fatally: " << mdbx_strerror(geo_rc);

  /* THE REGRESSION CHECK: the env must remain fully usable. An earlier version
   * of the fix refused with MDBX_BUSY on the implicit-shrink route, which
   * dxb_sync_locked() did not tolerate — it fell through to `fail:` and set
   * ENV_FATAL_ERROR, after which every call returned MDBX_PANIC. */
  MDBX_txn *txn = nullptr;
  const int begin_rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  EXPECT_NE(MDBX_PANIC, begin_rc) << "env was poisoned by the deferred resize";
  ASSERT_EQ(MDBX_SUCCESS, begin_rc) << "env must stay writable after a deferred resize";
  MDBX_val k = {(void *)"alive", 5}, v = {(void *)"yes", 3};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn)) << "post-resize commit must succeed";

  /* The still-open reader must keep reading its pinned snapshot. */
  EXPECT_GT(reader.reads_ok.load(std::memory_order_relaxed), 0)
      << "the pinned reader must survive the writer's grow and resize";

  reader.stop.store(true, std::memory_order_release);
  thr.join();
  EXPECT_EQ(MDBX_SUCCESS, reader.rc.load(std::memory_order_acquire)) << "reader abort must succeed";

  /* A final write proves the env is alive after the reader released its hold. */
  EXPECT_TRUE(fill(env, dbi, 5000, 5010, 'z'));

  mdbx_env_close(env);
  env_remove(kTestPath);
}

#if defined(_WIN32) || defined(_WIN64)
/* Windows-only: pin the deferral semantics deterministically. The first test
 * above only proves "returns, does not hang, does not poison"; on POSIX every
 * branch there is a no-op because readers never take remap_lock, so it would
 * also pass against the buggy code. This test drives the explicit-resize path
 * directly:
 *   - with a reader open, an explicit geometry change that needs the mapping
 *     re-created MUST return MDBX_UNABLE_EXTEND_MAPSIZE (deferred) — not SUCCESS
 *     and not a hang;
 *   - once the reader is closed, the SAME change MUST succeed.
 * That distinguishes the fixed behavior from both master (hang) and from a naive
 * "always succeed" implementation. The size assertions below additionally pin
 * that a deferred explicit resize leaves both the logical and the physical size
 * untouched (scenario C of issue #173). */
TEST(issue_gh0154, explicit_resize_defers_while_reader_open_then_succeeds) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue154-defer.mdbx";
  env_remove(testdb);

  MDBX_env *env = nullptr;
  MDBX_dbi dbi = 0;
  ASSERT_TRUE(open_env(&env, &dbi, testdb)) << "must open the env with a shrink-capable geometry";
  ASSERT_TRUE(fill(env, dbi, 0, 2000, 'x'));

  MDBX_envinfo info;
  std::memset(&info, 0, sizeof info);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof info));
  /* Ask for a smaller `upper`: this forces a remap (the mapping limit changes),
   * which needs the exclusive remap-lock. */
  const intptr_t new_upper = (intptr_t)info.mi_geo.current + (intptr_t)(512 << 10);
  ASSERT_LT((uint64_t)new_upper, info.mi_geo.upper) << "the requested upper must be a real reduction";

  /* Open a long-lived reader: on Windows it holds remap_lock shared. */
  reader_ctx reader = {env, dbi, nullptr, {false}, {false}, {0}, {0}};
  std::thread thr(reader_thread, &reader);
  while (!reader.started.load(std::memory_order_acquire))
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_EQ(MDBX_SUCCESS, reader.rc.load(std::memory_order_acquire)) << "reader must start";

  /* With the reader open the explicit resize must DEFER, returning
   * MDBX_UNABLE_EXTEND_MAPSIZE — never SUCCESS (that would mean it remapped
   * under a live reader) and never a hang. */
  const int deferred_rc = mdbx_env_set_geometry(env, -1, -1, new_upper, -1, -1, -1);
  EXPECT_EQ(MDBX_UNABLE_EXTEND_MAPSIZE, deferred_rc)
      << "an explicit remap with an open reader must defer with UNABLE_EXTEND_MAPSIZE, got: "
      << mdbx_strerror(deferred_rc);

  /* A deferred explicit resize must not write a meta nor touch the file: both
   * the logical and the physical size stay exactly as they were. */
  MDBX_envinfo deferred_info;
  std::memset(&deferred_info, 0, sizeof deferred_info);
  const int info_rc = mdbx_env_info_ex(env, nullptr, &deferred_info, sizeof deferred_info);
  EXPECT_EQ(MDBX_SUCCESS, info_rc) << "env_info_ex must work while the resize is deferred";
  EXPECT_EQ(info.mi_geo.current, deferred_info.mi_geo.current) << "logical size must be unchanged on deferral";
  EXPECT_EQ(info.mi_dxb_fsize, deferred_info.mi_dxb_fsize) << "physical file must be unchanged on deferral";

  /* The env must stay usable (not poisoned). */
  MDBX_txn *txn = nullptr;
  const int begin_rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn);
  EXPECT_NE(MDBX_PANIC, begin_rc) << "env was poisoned by the deferred resize";
  ASSERT_EQ(MDBX_SUCCESS, begin_rc);
  MDBX_val k = {(void *)"k", 1}, v = {(void *)"v", 1};
  EXPECT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));

  reader.stop.store(true, std::memory_order_release);
  thr.join();
  EXPECT_EQ(MDBX_SUCCESS, reader.rc.load(std::memory_order_acquire)) << "reader abort must succeed";

  /* Now that no reader holds the lock, the SAME explicit resize must succeed. */
  const int applied_rc = mdbx_env_set_geometry(env, -1, -1, new_upper, -1, -1, -1);
  EXPECT_EQ(MDBX_SUCCESS, applied_rc) << "the deferred resize must apply once the reader is gone, got: "
                                      << mdbx_strerror(applied_rc);

  std::memset(&info, 0, sizeof info);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof info));
  EXPECT_EQ((uint64_t)new_upper, info.mi_geo.upper) << "the new upper must be in effect";

  mdbx_env_close(env);
  env_remove(testdb);
}
#endif /* Windows */
