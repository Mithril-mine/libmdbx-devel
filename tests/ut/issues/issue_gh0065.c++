#include "../../../src/internals.h"
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/65
// On Windows (and any build with MDBX_AVOID_MSYNC=1 and MDBX_MMAP_NEEDS_JOLT=1)
// every SAFE_NOSYNC/NOMETASYNC WRITEMAP commit flushed the whole allocated
// mapping: dxb_sync_locked() step-1 selected the msync path on MDBX_WRITEMAP
// alone, ignoring MDBX_AVOID_MSYNC, and osal_msync() did not short-circuit the
// MDBX_SYNC_NONE case because MDBX_MMAP_NEEDS_JOLT is 1 on Windows. The
// expensive whole-mapping FlushViewOfFile is exactly the cost that no-sync
// modes exist to avoid, and MDBX_AVOID_MSYNC=1 is the Windows design that
// persists dirty pages by explicit writes instead.
// The test counts mi_pgop_stat.msync over a batch of no-sync WRITEMAP commits:
// the counter grows per FlushViewOfFile on the affected builds, so it must
// stay small (only part of the legitimate growth syncs), not ~one per commit.
// NB: on builds where write-mapped memory needs the explicit MS_ASYNC "jolt"
// (MDBX_MMAP_NEEDS_JOLT=1) and persistence is not routed through explicit
// writes (MDBX_AVOID_MSYNC=0), e.g. macOS/BSD, one cheap async msync per
// no-sync commit is the designed behavior, so only the durability (fsync)
// count is bounded there; the strict msync bound applies to Linux and to the
// Windows-with-AVOID_MSYNC path that the fix preserves.

TEST(issue_gh0065, writemap_nosync_commits_do_not_flush_whole_mapping) {
  const char *const testdb = "issue65.mdbx";
  remove(testdb);

  const int N = 200;
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, 64 << 20, -1, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR | MDBX_WRITEMAP | MDBX_SAFE_NOSYNC, 0644));

  MDBX_envinfo ei;
  std::memset(&ei, 0, sizeof(ei));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &ei, sizeof(ei)));
  const uint64_t msync0 = ei.mi_pgop_stat.msync;
  const uint64_t fsync0 = ei.mi_pgop_stat.fsync;

  for (int i = 0; i < N; i++) {
    MDBX_txn *txn = nullptr;
    MDBX_dbi dbi = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
    char keybuf[32], databuf[32];
    int kn = std::snprintf(keybuf, sizeof keybuf, "k%08d", i);
    int vn = std::snprintf(databuf, sizeof databuf, "v%08d", i);
    MDBX_val key = {keybuf, size_t(kn)};
    MDBX_val data = {databuf, size_t(vn)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &data, MDBX_UPSERT));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &ei, sizeof(ei)));
  const uint64_t msync_delta = ei.mi_pgop_stat.msync - msync0;
  const uint64_t fsync_delta = ei.mi_pgop_stat.fsync - fsync0;

  /* Before the fix a per-commit whole-mapping flush made msync_delta ~N on
   * Windows; after it the real syncs are routed through fsync (os mdbx_fsync)
   * and osal_msync/SYNC_NONE is a no-op, so both deltas only reflect the
   * legitimate size/data syncs on the few file-growth events. */
  if (MDBX_AVOID_MSYNC == 0 && MDBX_MMAP_NEEDS_JOLT != 0) {
    /* jolt platforms (e.g. macOS/BSD): the per-commit async msync "kick" is
     * by design and cheap, but the durability syncs must stay small. */
    EXPECT_LE(fsync_delta, uint64_t(N / 4));
  } else {
    EXPECT_LE(msync_delta, uint64_t(N / 4));
    EXPECT_LE(msync_delta + fsync_delta, uint64_t(N / 4));
  }

  /* sanity: all records must survive a clean close and a read-only reopen */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_close(env));
  env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR | MDBX_RDONLY, 0644));
  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_db_flags_t(0), &dbi));
  MDBX_cursor *cursor = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_cursor_open(txn, dbi, &cursor));
  MDBX_val key = {}, data = {};
  int count = 0;
  while (mdbx_cursor_get(cursor, &key, &data, MDBX_NEXT) == MDBX_SUCCESS)
    count++;
  mdbx_cursor_close(cursor);
  mdbx_txn_abort(txn);
  mdbx_env_close(env);
  EXPECT_EQ(N, count);
  remove(testdb);
}
