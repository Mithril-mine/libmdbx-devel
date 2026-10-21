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

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/68
// mdbx_cursor_count_ex() accepts both the full sizeof(MDBX_stat) and the
// legacy pre-ms_mod_txnid size (offsetof(MDBX_stat, ms_mod_txnid), i.e. 40
// bytes) as the `bytes` parameter, but cleared the whole struct with
// `memset(ns, 0, sizeof(*ns))` — an unconditional 48-byte write into a buffer
// the caller sized for the accepted 40-byte legacy value. The 8 bytes written
// past the caller's buffer could silently corrupt adjacent data or crash the
// process. The function must clear (and fill) only the bytes the caller
// provided.

static const uint64_t CANARY = UINT64_C(0xfeedfacecafef00d);

/* full-size view: canary right after MDBX_stat */
struct mdbx_stat_canary {
  MDBX_stat stat;
  uint64_t canary;
};

static void remove_db(const char *path) {
  try {
    mdbx::env::remove(path);
  } catch (const mdbx::error &) {
    /* the test database is not required to exist beforehand */
  }
}

static MDBX_env *open_dupsort_env(const char *path, MDBX_dbi *dbi_out) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return nullptr;
  if (mdbx_env_set_geometry(env, -1, -1, 16 << 20, -1, -1, -1) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  if (mdbx_env_set_maxdbs(env, 8) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  if (mdbx_env_open(env, path, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }

  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_txn_flags_t(0), &txn) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  MDBX_dbi dbi = 0;
  if (mdbx_dbi_open(txn, "multi", MDBX_DUPSORT | MDBX_CREATE, &dbi) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    mdbx_env_close(env);
    return nullptr;
  }
  MDBX_val key = {const_cast<char *>("key"), 3};
  MDBX_val first = {const_cast<char *>("first"), 5};
  MDBX_val second = {const_cast<char *>("second"), 6};
  if (mdbx_put(txn, dbi, &key, &first, MDBX_UPSERT) != MDBX_SUCCESS ||
      mdbx_put(txn, dbi, &key, &second, MDBX_APPENDDUP) != MDBX_SUCCESS || mdbx_txn_commit(txn) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    mdbx_env_close(env);
    return nullptr;
  }
  *dbi_out = dbi;
  return env;
}

static MDBX_cursor *positioned_cursor(MDBX_env *env, MDBX_dbi dbi, MDBX_txn **txn_out) {
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    return nullptr;
  MDBX_cursor *mc = nullptr;
  if (mdbx_cursor_open(txn, dbi, &mc) != MDBX_SUCCESS) {
    mdbx_txn_abort(txn);
    return nullptr;
  }
  MDBX_val key = {const_cast<char *>("key"), 3};
  MDBX_val data = {};
  if (mdbx_cursor_get(mc, &key, &data, MDBX_FIRST) != MDBX_SUCCESS) {
    mdbx_cursor_close(mc);
    mdbx_txn_abort(txn);
    return nullptr;
  }
  *txn_out = txn;
  return mc;
}

TEST(issue_gh0068, legacy_size_does_not_write_past_buffer) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue68-legacy.mdbx";
  remove_db(testdb);
  MDBX_dbi dbi = 0;
  MDBX_env *env = open_dupsort_env(testdb, &dbi);
  ASSERT_NE(env, nullptr);
  MDBX_txn *rtxn = nullptr;
  MDBX_cursor *mc = positioned_cursor(env, dbi, &rtxn);
  ASSERT_NE(mc, nullptr);

  const size_t legacy = offsetof(MDBX_stat, ms_mod_txnid);
  ASSERT_EQ(legacy, 40u);

  /* legacy view: a 40-byte data region with a canary right after it — the
   * pre-fix 48-byte memset clobbers exactly those canary bytes */
  struct legacy_probe {
    alignas(MDBX_stat) uint8_t data[40];
    uint64_t canary;
  } probe;
  std::memset(&probe, 0, sizeof(probe));
  probe.canary = CANARY;

  size_t count = 0;
  const int rc = mdbx_cursor_count_ex(mc, &count, (MDBX_stat *)probe.data, legacy);
  EXPECT_EQ(MDBX_SUCCESS, rc);
  EXPECT_EQ(2u, count);
  EXPECT_EQ(2u, ((const MDBX_stat *)(const void *)probe.data)->ms_entries);
  EXPECT_EQ(CANARY, probe.canary);

  mdbx_cursor_close(mc);
  mdbx_txn_abort(rtxn);
  mdbx_env_close(env);
  remove_db(testdb);
}

TEST(issue_gh0068, exact_heap_buffer_is_not_overflowed) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue68-heap.mdbx";
  remove_db(testdb);
  MDBX_dbi dbi = 0;
  MDBX_env *env = open_dupsort_env(testdb, &dbi);
  ASSERT_NE(env, nullptr);
  MDBX_txn *rtxn = nullptr;
  MDBX_cursor *mc = positioned_cursor(env, dbi, &rtxn);
  ASSERT_NE(mc, nullptr);

  const size_t legacy = offsetof(MDBX_stat, ms_mod_txnid);
  ASSERT_EQ(legacy, 40u);
  /* the exact heap-buffer size from the issue's reproducer: pre-fix this is
   * an 8-byte heap-buffer-overflow (detected by ASan or by the allocator) */
  void *buf = std::malloc(legacy);
  ASSERT_NE(buf, nullptr);
  std::memset(buf, 0, legacy);

  size_t count = 0;
  const int rc = mdbx_cursor_count_ex(mc, &count, (MDBX_stat *)buf, legacy);
  EXPECT_EQ(MDBX_SUCCESS, rc);
  EXPECT_EQ(2u, count);

  std::free(buf);
  mdbx_cursor_close(mc);
  mdbx_txn_abort(rtxn);
  mdbx_env_close(env);
  remove_db(testdb);
}

TEST(issue_gh0068, full_size_fills_all_fields_within_buffer) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue68-full.mdbx";
  remove_db(testdb);
  MDBX_dbi dbi = 0;
  MDBX_env *env = open_dupsort_env(testdb, &dbi);
  ASSERT_NE(env, nullptr);
  MDBX_txn *rtxn = nullptr;
  MDBX_cursor *mc = positioned_cursor(env, dbi, &rtxn);
  ASSERT_NE(mc, nullptr);

  mdbx_stat_canary probe;
  std::memset(&probe, 0, sizeof(probe));
  probe.canary = CANARY;

  size_t count = 0;
  const int rc = mdbx_cursor_count_ex(mc, &count, &probe.stat, sizeof(MDBX_stat));
  EXPECT_EQ(MDBX_SUCCESS, rc);
  EXPECT_EQ(2u, count);
  EXPECT_EQ(2u, probe.stat.ms_entries);
  /* the canary sits right after MDBX_stat and must stay intact */
  EXPECT_EQ(CANARY, probe.canary);

  mdbx_cursor_close(mc);
  mdbx_txn_abort(rtxn);
  mdbx_env_close(env);
  remove_db(testdb);
}

TEST(issue_gh0068, invalid_bytes_are_rejected) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue68-invalid.mdbx";
  remove_db(testdb);
  MDBX_dbi dbi = 0;
  MDBX_env *env = open_dupsort_env(testdb, &dbi);
  ASSERT_NE(env, nullptr);
  MDBX_txn *rtxn = nullptr;
  MDBX_cursor *mc = positioned_cursor(env, dbi, &rtxn);
  ASSERT_NE(mc, nullptr);

  mdbx_stat_canary probe;
  std::memset(&probe, 0, sizeof(probe));
  probe.canary = CANARY;

  size_t count = 0;
  /* neither the legacy-1 nor zero is an accepted size: EINVAL before any write */
  EXPECT_EQ(MDBX_EINVAL, mdbx_cursor_count_ex(mc, &count, &probe.stat, sizeof(MDBX_stat) - 1));
  EXPECT_EQ(MDBX_EINVAL, mdbx_cursor_count_ex(mc, &count, &probe.stat, 0));
  EXPECT_EQ(CANARY, probe.canary);

  mdbx_cursor_close(mc);
  mdbx_txn_abort(rtxn);
  mdbx_env_close(env);
  remove_db(testdb);
}
