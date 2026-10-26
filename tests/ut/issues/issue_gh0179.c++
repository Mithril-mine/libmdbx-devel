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

#include "../../../src/internals.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/179
//
// On Windows, MDBX_EXCLUSIVE used to silently switch the durable write path:
// the overlapped write-through handle and the dsync handle were not opened at
// all (their opening was skipped together with RDONLY/NOSYNC modes) and the
// data file was opened with ShareMode == 0, so no second handle from the same
// process was possible. As a result an exclusive durable environment committed
// through lazy writes + FlushFileBuffers (~2x slower in the reporter's
// benchmark) while POSIX exclusive kept the normal fast path.
//
// The fix (behind the MDBX_ENABLE_EXCLUSIVE_FASTPATH build option, default ON
// for Windows) opens the data-file handles shareable in exclusive mode and
// enforces exclusivity with constant byte-range locks instead: the meta region
// is locked through fd4meta and the body region through fd4data (the very
// handles that write those regions), and the lock file stays unshareable.
//
// Invariants pinned here:
//   1. (Windows, fastpath build) an exclusive durable env uses the normal
//      i/o path: both the overlapped and the dsync handles are open;
//   2. a second exclusive open while the first env is alive fails with exactly
//      MDBX_BUSY (on unfixed Windows it returned the raw ERROR_SHARING_VIOLATION
//      winerror 32, violating the documented mdbx_env_open() contract);
//   3. data written by an exclusive durable env survives close/reopen and
//      passes MDBX_VALIDATION checks (the constant locks do not break the
//      write/resize/flush paths);
//   4. transactions that weaken durability per-txn (MDBX_TXN_NOSYNC,
//      MDBX_TXN_NOMETASYNC at begin or via mdbx_txn_checkpoint()) work under
//      the constant locks: weak metas/durable commits interleave without
//      LOCK_VIOLATION and all data survives validation;
//   5. (Windows) an exclusive open through a different hard link — a distinct
//      lock file, so the unshareable-lck protection does not apply — is still
//      rejected with MDBX_BUSY via the constant body lock, not a raw winerror.
//
// Portability: runs on every platform; invariant 1 is Windows-only (guarded),
// invariants 2-3 hold on POSIX too (sanity). Following the issue_gh0154
// precedent, the Windows semantics are exercised by the Windows CI cells.

namespace {

const char *const kDbPath = "issue179.mdbx";

#if defined(_WIN32) || defined(_WIN64)
/* Detect Wine locally instead of reading the library-private `globals` symbol,
 * which is not exported from the shared library (an unresolved-symbol link
 * error with MDBX_BUILD_SHARED_LIBRARY=ON). Same technique as windows_import():
 * Wine's ntdll.dll exports wine_get_version(). */
static bool running_under_wine() {
  static int cached = -1;
  if (cached < 0)
    cached = GetProcAddress(GetModuleHandleA("ntdll.dll"), "wine_get_version") ? 1 : 0;
  return cached != 0;
}
#endif

static void cleanup(const char *path) {
  std::string lck = std::string(path) + "-lck";
  std::remove(path);
  std::remove(lck.c_str());
}

static MDBX_env *open_env(const char *path, MDBX_env_flags_t extra_flags) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return nullptr;
  mdbx_env_set_geometry(env, -1, -1, 64 << 20, -1, -1, -1);
  const int rc = mdbx_env_open(env, path, MDBX_NOSUBDIR | extra_flags, 0644);
  if (rc != MDBX_SUCCESS) {
    fprintf(stderr, "issue179: open(%s, 0x%x) -> %s (%d)\n", path, (unsigned)extra_flags, mdbx_strerror(rc), rc);
    mdbx_env_close(env);
    return nullptr;
  }
  return env;
}

/* Write a batch of key-value pairs with reasonably large values so the commit
 * path exercises both single-page and overflow writes. */
static void write_batch(MDBX_env *env, MDBX_dbi dbi, size_t from, size_t to) {
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  std::vector<char> val(3400, 'v');
  char kbuf[32];
  for (size_t i = from; i < to; ++i) {
    int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", i);
    MDBX_val k{kbuf, size_t(kn)};
    MDBX_val v{val.data(), val.size()};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
}

static MDBX_dbi open_dbi(MDBX_env *env) {
  MDBX_txn *txn = nullptr;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  return dbi;
}

TEST(issue_gh0179, exclusive_durable_keeps_the_fast_iopath) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  cleanup(kDbPath);

  MDBX_env *env = open_env(kDbPath, MDBX_EXCLUSIVE | MDBX_SYNC_DURABLE);
  ASSERT_NE(nullptr, env);

#if defined(_WIN32) || defined(_WIN64)
  /* With MDBX_ENABLE_EXCLUSIVE_FASTPATH (default ON for Windows) an exclusive
   * durable env must open the very same handles as a non-exclusive one:
   * the overlapped write-through handle for the body and the dsync handle for
   * meta. On unfixed code both are skipped in exclusive mode. */
  if (MDBX_ENABLE_EXCLUSIVE_FASTPATH && !running_under_wine()) {
    EXPECT_NE(0u, (unsigned)(uintptr_t)env->ioring.overlapped_fd)
        << "exclusive durable must keep the overlapped write-through handle";
    EXPECT_NE(INVALID_HANDLE_VALUE, env->dsync_fd) << "exclusive durable must keep the dsync handle";
    /* fd4meta aliases dsync_fd only while MDBX_NOMETASYNC is off (env_open
     * points it back to lazy_fd otherwise); this env is opened durable without
     * NOMETASYNC, so meta must go through the dsync handle. */
    EXPECT_EQ(env->dsync_fd, env->fd4meta) << "meta must be written through the dsync handle";
  }
#endif

  /* The env must be fully operational: write, then verify data and counters. */
  const MDBX_dbi dbi = open_dbi(env);
  write_batch(env, dbi, 0, 200);

  MDBX_envinfo info;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof info));
  /* Just check that data pages were allocated beyond the meta pages; the exact
   * count is pagesize-dependent, so it must not be compared to a constant. The
   * real integrity is verified by the VALIDATION re-open below. */
  EXPECT_GT(info.mi_last_pgno, (pgno_t)(NUM_METAS - 1)) << "the batch must have allocated data pages";

  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));

  /* Data must survive; re-open cooperatively (non-exclusive) and validate. */
  MDBX_env *check = open_env(kDbPath, MDBX_VALIDATION);
  ASSERT_NE(nullptr, check);
  MDBX_txn *ro = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(check, nullptr, MDBX_TXN_RDONLY, &ro));
  char kbuf[32];
  for (size_t i = 0; i < 200; i += 37) {
    int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", i);
    MDBX_val k{kbuf, size_t(kn)}, v;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_get(ro, dbi, &k, &v)) << "key " << kbuf << " must survive exclusive durable writes";
    ASSERT_EQ(3400u, v.iov_len);
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(ro));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(check));
  cleanup(kDbPath);
}

TEST(issue_gh0179, second_exclusive_open_returns_busy) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  cleanup(kDbPath);

  MDBX_env *first = open_env(kDbPath, MDBX_EXCLUSIVE | MDBX_SYNC_DURABLE);
  ASSERT_NE(nullptr, first);
  const MDBX_dbi dbi = open_dbi(first);
  write_batch(first, dbi, 0, 50);

  /* A second exclusive open must fail with exactly MDBX_BUSY, as documented
   * for mdbx_env_open(). On unfixed Windows this returned the raw
   * ERROR_SHARING_VIOLATION (32) from the unshareable data file.
   * On POSIX the exact code is platform-dependent (EAGAIN for a contended
   * fcntl lock), so only "must not succeed" is asserted there — the
   * MDBX_BUSY contract pinned here is the Windows behavior. */
  MDBX_env *second = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&second));
  mdbx_env_set_geometry(second, -1, -1, 64 << 20, -1, -1, -1);
  const int rc2 = mdbx_env_open(second, kDbPath, MDBX_NOSUBDIR | MDBX_EXCLUSIVE | MDBX_SYNC_DURABLE, 0644);
#if defined(_WIN32) || defined(_WIN64)
  EXPECT_EQ((int)MDBX_BUSY, rc2) << "second exclusive open must report MDBX_BUSY, got: " << mdbx_strerror(rc2);
#else
  EXPECT_NE((int)MDBX_SUCCESS, rc2) << "second exclusive open must fail, got: " << mdbx_strerror(rc2);
#endif
  mdbx_env_close(second);

  /* A cooperative (non-exclusive) open must be rejected too. */
  MDBX_env *coop = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&coop));
  mdbx_env_set_geometry(coop, -1, -1, 64 << 20, -1, -1, -1);
  const int rc3 = mdbx_env_open(coop, kDbPath, MDBX_NOSUBDIR, 0644);
  EXPECT_NE((int)MDBX_SUCCESS, rc3) << "cooperative open must not succeed while an exclusive env is alive";
  mdbx_env_close(coop);

  /* The first env must remain fully operational after the failed attempts. */
  write_batch(first, dbi, 50, 100);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(first));
  cleanup(kDbPath);
}

TEST(issue_gh0179, weakened_txns_work_under_exclusive_durable_env) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  cleanup(kDbPath);

  /* An env opened MDBX_EXCLUSIVE|MDBX_SYNC_DURABLE must accept transactions
   * that weaken durability per-txn (MDBX_TXN_NOSYNC / MDBX_TXN_NOMETASYNC at
   * begin, or via mdbx_txn_checkpoint()) while the constant meta/body locks
   * are held: weak metas go through fd4meta and data through fd4data — the
   * very lock owners — so no LOCK_VIOLATION may occur, and the interleaving
   * of weak and durable commits must keep the previous-weak-meta sync logic
   * (need_flush_for_nometasync / meta_sync) working. */
  MDBX_env *env = open_env(kDbPath, MDBX_EXCLUSIVE | MDBX_SYNC_DURABLE);
  ASSERT_NE(nullptr, env);
  const MDBX_dbi dbi = open_dbi(env);

  /* txn 0: durable baseline */
  write_batch(env, dbi, 0, 50);

  /* txn 1: weakened to NOSYNC at begin */
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_NOSYNC, &txn));
    EXPECT_TRUE(mdbx_txn_flags(txn) & MDBX_TXN_NOSYNC);
    std::vector<char> val(3400, 'n');
    MDBX_val v{val.data(), val.size()};
    char kbuf[32];
    for (size_t i = 100; i < 150; ++i) {
      int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", i);
      MDBX_val k{kbuf, size_t(kn)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  /* txn 2: durable again — must sync the previous weak meta */
  write_batch(env, dbi, 200, 250);

  /* txn 3: weakened to NOMETASYNC at begin */
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_NOMETASYNC, &txn));
    EXPECT_TRUE(mdbx_txn_flags(txn) & MDBX_TXN_NOMETASYNC);
    std::vector<char> val(3400, 'm');
    MDBX_val v{val.data(), val.size()};
    char kbuf[32];
    for (size_t i = 300; i < 350; ++i) {
      int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", i);
      MDBX_val k{kbuf, size_t(kn)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  /* txn 4: durable — again syncs the previous weak meta */
  write_batch(env, dbi, 400, 450);

  /* txn 5: weakening mid-flight via mdbx_txn_checkpoint() */
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    std::vector<char> val(3400, 'c');
    MDBX_val v{val.data(), val.size()};
    char kbuf[32];
    for (size_t i = 500; i < 525; ++i) {
      int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", i);
      MDBX_val k{kbuf, size_t(kn)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
    }
    int rc = mdbx_txn_checkpoint(txn, MDBX_TXN_NOSYNC, nullptr);
    ASSERT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) << mdbx_strerror(rc);
    for (size_t i = 525; i < 550; ++i) {
      int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", i);
      MDBX_val k{kbuf, size_t(kn)};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
    }
    rc = mdbx_txn_checkpoint(txn, MDBX_TXN_NOMETASYNC, nullptr);
    ASSERT_TRUE(rc == MDBX_SUCCESS || rc == MDBX_RESULT_TRUE) << mdbx_strerror(rc);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  /* txn 6: final durable commit, then close (which syncs too) */
  write_batch(env, dbi, 600, 650);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));

  /* Everything committed — including via the weakened txns — must survive and
   * pass the full validation on reopen. */
  MDBX_env *check = open_env(kDbPath, MDBX_VALIDATION);
  ASSERT_NE(nullptr, check);
  MDBX_txn *ro = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(check, nullptr, MDBX_TXN_RDONLY, &ro));
  char kbuf[32];
  const size_t probes[] = {0, 49, 100, 149, 200, 249, 300, 349, 400, 449, 500, 549, 600, 649};
  for (size_t i : probes) {
    int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", i);
    MDBX_val k{kbuf, size_t(kn)}, v;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_get(ro, dbi, &k, &v)) << "key " << kbuf << " lost after weakened commits";
    ASSERT_EQ(3400u, v.iov_len);
  }
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(ro));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(check));
  cleanup(kDbPath);
}

TEST(issue_gh0179, exclusive_geometry_changes_work_under_constant_locks) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  cleanup(kDbPath);

  /* Exercise file growth (resize via SetEndOfFile through lazy_fd) and bulk
   * writes while the constant meta/body locks are held: both must work. */
  MDBX_env *env = open_env(kDbPath, MDBX_EXCLUSIVE | MDBX_SYNC_DURABLE);
  ASSERT_NE(nullptr, env);
  const MDBX_dbi dbi = open_dbi(env);
  write_batch(env, dbi, 0, 2000); /* grows the file well beyond the initial size */

  /* Shrink then grow again through the explicit geometry API. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 32 << 20, -1, -1, -1));
  write_batch(env, dbi, 2000, 2100);

  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));

  MDBX_env *check = open_env(kDbPath, MDBX_VALIDATION);
  ASSERT_NE(nullptr, check);
  MDBX_txn *ro = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(check, nullptr, MDBX_TXN_RDONLY, &ro));
  char kbuf[32];
  int kn = std::snprintf(kbuf, sizeof kbuf, "k%07zu", size_t(2099));
  MDBX_val k{kbuf, size_t(kn)}, v;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_get(ro, dbi, &k, &v));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_abort(ro));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(check));
  cleanup(kDbPath);
}

#if defined(_WIN32) || defined(_WIN64)
/* An exclusive open through a *different hard link* to the same data file uses
 * a different lock file, so the unshareable-lock-file protection does not
 * apply. The constant body lock must still reject it: the header read through
 * lazy_fd hits ERROR_LOCK_VIOLATION, which must be normalized to MDBX_BUSY
 * rather than surfacing the raw winerror. On POSIX per-process fcntl locks do
 * not conflict with themselves, so this case is Windows-only (the behavior
 * there matches master, where the unshareable data file rejected it). */
TEST(issue_gh0179, exclusive_via_hardlink_is_rejected_with_busy) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  cleanup(kDbPath);
  const std::string link = std::string(kDbPath) + ".link";
  std::remove(link.c_str());
  std::string link_lck = link + "-lck";
  std::remove(link_lck.c_str());

  MDBX_env *first = open_env(kDbPath, MDBX_EXCLUSIVE | MDBX_SYNC_DURABLE);
  ASSERT_NE(nullptr, first);
  const MDBX_dbi dbi = open_dbi(first);
  write_batch(first, dbi, 0, 50);

  /* Create a hard link to the data file. NTFS supports hard links; if the
   * filesystem does not (e.g. some CI scratch volumes), skip rather than fail. */
  if (!CreateHardLinkA(link.c_str(), kDbPath, nullptr)) {
    mdbx_env_close(first);
    cleanup(kDbPath);
    GTEST_SKIP() << "the filesystem does not support hard links";
  }

  MDBX_env *second = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&second));
  mdbx_env_set_geometry(second, -1, -1, 64 << 20, -1, -1, -1);
  const int rc = mdbx_env_open(second, link.c_str(), MDBX_NOSUBDIR | MDBX_EXCLUSIVE | MDBX_SYNC_DURABLE, 0644);
  EXPECT_EQ((int)MDBX_BUSY, rc) << "exclusive open via a hard link must report MDBX_BUSY, got: " << mdbx_strerror(rc);
  mdbx_env_close(second);

  /* The first env must stay fully operational. */
  write_batch(first, dbi, 50, 100);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(first));

  std::remove(link.c_str());
  std::remove(link_lck.c_str());
  cleanup(kDbPath);
}
#endif /* Windows */

} // namespace
