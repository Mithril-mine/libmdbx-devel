#include "mdbx.h++"
#undef MDBX_DEPRECATED /* avoid redefinition by the internal headers below */
#include "src/layout-lck.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/81
// The writer's oldest-snapshot computation trusts the shared cached_oldest_txnid
// value from the (externally-writable) lock mapping while the reader table is
// unchanged, so a forged/corrupted cached value beyond the steady snapshot is
// used as the true oldest-reader: the writer then reuses pages still referenced
// by live readers. The fix re-clamps the cache by forcing a full reader-table
// rescan whenever cached_oldest_txnid exceeds the steady snapshot. The test
// poisons cached_oldest_txnid (and prepares the unchanged-table fast-path by
// writing the "nothing changed" refresh signature) in the MAP_SHARED lock file,
// then triggers the writer's oldest-snapshot computation: the poisoned value
// must be re-clamped to the sane one.
//
// The test is POSIX-only: on Windows the environment keeps the lower region of
// the lock file (up to rdt_length) byte-range locked while open, so a second
// handle in the same process cannot write to cached_oldest_txnid at all.

// The reader-table refresh flag value that forces skipping the reader-table
// rescan is the 'None' tetra: MDBX_TETRAD('N','o','n','e').
static const uint32_t nothing_changed_signature = MDBX_TETRAD('N', 'o', 'n', 'e');

static void write_raw(std::fstream &f, std::streamoff off, const void *data, size_t size) {
  f.seekp(off, std::ios::beg);
  f.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
  ASSERT_TRUE(f.good());
}

static void read_raw(std::fstream &f, std::streamoff off, void *data, size_t size) {
  f.seekg(off, std::ios::beg);
  f.read(static_cast<char *>(data), static_cast<std::streamsize>(size));
  ASSERT_TRUE(f.good());
}

TEST(issue_gh0081, poisoned_cached_oldest_reclamped) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const std::string testdb = "issue81.mdbx";
  const std::string lck_path = testdb + "-lck";
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  std::remove(lck_path.c_str());

  constexpr uint32_t pagesize = 4096;
  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 4));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxreaders(env, 8));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, pagesize));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644)) << "the base environment must open";

  /* One committed write transaction so the steady snapshot is sane. */
  MDBX_dbi dbi;
  {
    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "kv", MDBX_CREATE, &dbi));
    for (int i = 0; i < 4; ++i) {
      const std::string key = "key" + std::to_string(i);
      const std::string val = "value" + std::to_string(i);
      MDBX_val key_val = {const_cast<char *>(key.data()), key.size()};
      MDBX_val val_val = {const_cast<char *>(val.data()), val.size()};
      ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key_val, &val_val, MDBX_UPSERT));
    }
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  }

  /* The lock file is memory-mapped MAP_SHARED, so a write to the file is
   * visible in the environment's mapping. Poison cached_oldest_txnid with a
   * value far beyond any sane steady snapshot (but inside the valid txnid
   * range, so it looks plausible), and pin the reader-table refresh flag to
   * the "nothing changed" signature so the unchanged-table fast-path, which
   * blindly trusts the poisoned cache, is taken. */
  const uint64_t poisoned = UINT64_C(0x7FFFFFFFFFFFFFFF);
  const size_t oldest_off = offsetof(lck_t, cached_oldest_txnid);
  const size_t flag_off = offsetof(lck_t, rdt_refresh_flag);
  {
    std::fstream f(lck_path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open()) << "must be able to open the lock file";
    write_raw(f, static_cast<std::streamoff>(oldest_off), &poisoned, sizeof(poisoned));
    write_raw(f, static_cast<std::streamoff>(flag_off), &nothing_changed_signature, sizeof(nothing_changed_signature));
    f.flush();
    f.close();
  }

  /* Trigger the writer's oldest-snapshot computation: mdbx_env_set_geometry()
   * forces txn_gc_detent() -> mvcc_snapshot_oldest_rw(), the code path that
   * learns the true oldest-reader before the writer reuses retired pages. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, -1))
      << "re-clamping requires a successful writer snapshot computation";

  /* The poisoned cached value must have been rejected and re-clamped to the
   * real oldest-reader (i.e. the steady snapshot, since no live reader holds
   * an older one) instead of being trusted as-is. */
  uint64_t cached = 0;
  {
    std::fstream f(lck_path, std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open()) << "must be able to reopen the lock file";
    read_raw(f, static_cast<std::streamoff>(oldest_off), &cached, sizeof(cached));
    f.close();
  }
  EXPECT_NE(cached, poisoned) << "cached = 0x" << std::hex << cached << " poisoned = 0x" << poisoned;
  EXPECT_LT(cached, UINT64_C(1) << 40) << "cached = 0x" << std::hex << cached;

  mdbx_env_close(env);
  mdbx::env::remove(testdb);
  std::remove(lck_path.c_str());
}