#include "mdbx.h++"
#undef MDBX_DEPRECATED /* avoid redefinition by the internal headers below */
#include "src/layout-lck.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/79
// The shared reader-table occupancy counter (rdt_length) lives in the
// externally-writable lock file and is read without clamping to max_readers at
// every reader-table scan, so a too-large value drives out-of-bounds reads of
// the fixed-size rdt[] array (and, on 32-bit, a heap overflow in
// mvcc_cleanup_dead()). The test opens an environment, writes a too-large
// rdt_length into its (MAP_SHARED) lock file, and runs mdbx_reader_list(),
// which scans the whole table: it must clamp the count to max_readers instead
// of walking past the array.

static void write_u32_le(uint8_t *p, uint32_t v) {
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

static int reader_list_cb(void *, int, int, mdbx_pid_t, mdbx_tid_t, uint64_t, uint64_t, size_t, size_t) noexcept {
  return 0;
}

static void poison_rdt_length(const std::string &lck_path) {
  /* The lock file is memory-mapped MAP_SHARED, so a write to the file is
   * visible in the environment's mapping. Poison rdt_length with a value far
   * beyond max_readers. */
  const size_t rdt_length_off = offsetof(lck_t, rdt_length);
  std::fstream f(lck_path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(f.is_open()) << "must be able to open the lock file";
  f.seekp((std::streamoff)(rdt_length_off), std::ios::beg);
  uint8_t value[4];
  write_u32_le(value, 0x40000000u);
  f.write(reinterpret_cast<char *>(value), sizeof(value));
  ASSERT_TRUE(f.good());
  f.flush();
  f.close();
}

static MDBX_env *create_open_env(const std::string &testdb, const std::string &lck_path) {
  try {
    mdbx::env::remove(testdb);
  } catch (const mdbx::error &) {
  }
  std::remove(lck_path.c_str());

  constexpr uint32_t pagesize = 4096;
  MDBX_env *env = nullptr;
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_maxreaders(env, 8));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, pagesize));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb.c_str(), MDBX_NOSUBDIR, 0644)) << "the base environment must open";
  return env;
}

TEST(issue_gh0079, oversized_rdt_length_clamped) {
  const std::string testdb = "issue79.mdbx";
  const std::string lck_path = testdb + "-lck";
  MDBX_env *env = create_open_env(testdb, lck_path);
  ASSERT_NE(nullptr, env);

  poison_rdt_length(lck_path);

  /* mdbx_reader_list() scans every reader-table slot; the poisoned count must
   * be clamped to max_readers instead of walking out of the array. */
  EXPECT_EQ(MDBX_RESULT_TRUE, mdbx_reader_list(env, reader_list_cb, nullptr))
      << "rdt_length beyond max_readers must be clamped, not scanned out of bounds";

  mdbx_env_close(env);
  mdbx::env::remove(testdb);
  std::remove(lck_path.c_str());
}

TEST(issue_gh0079, env_info_clamps_rdt_length) {
  const std::string testdb = "issue79-envinfo.mdbx";
  const std::string lck_path = testdb + "-lck";
  MDBX_env *env = create_open_env(testdb, lck_path);
  ASSERT_NE(nullptr, env);

  poison_rdt_length(lck_path);

  /* mdbx_env_info_ex() must also clamp the reader-table scan to max_readers:
   * before the fix it stored the raw rdt_length into mi_numreaders and walked
   * past the end of the fixed-sized reader table. */
  MDBX_envinfo info;
  memset(&info, 0, sizeof(info));
  EXPECT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)))
      << "the poisoned rdt_length must be clamped, not scanned out of bounds";
  EXPECT_LE(info.mi_numreaders, info.mi_maxreaders) << "mi_numreaders must not exceed max_readers";

  mdbx_env_close(env);
  mdbx::env::remove(testdb);
  std::remove(lck_path.c_str());
}
