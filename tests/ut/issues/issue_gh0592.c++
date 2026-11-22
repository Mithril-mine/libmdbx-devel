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

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/592
//
// txn_setup_primal() compares the snapshot of a starting read transaction with the datafile mapping without the
// remap-lock and then asks dxb_resize() to grow the mapping to the size of that snapshot. Meanwhile a write transaction
// of another thread of the same process may grow the mapping further, allocating pages at the end of the datafile. The
// stale request then reduced the mapping back to the reader's size (on POSIX also the recorded file size, without
// truncating the file), and the writer kept allocating pages beyond the mapping as the process tracks it: with
// MDBX_WRITEMAP and prefault-write the next allocation crashed in mincore_fetch() on alloca() of a negative length
// (SIGBUS/SIGSEGV).
//
// The race needs a precise interleaving, so the test issues the reader's stale request directly through the testing
// API, from another thread, while the writer holds the grown mapping. The request must not reduce the mapping, and
// the writer must go on allocating at the end of the datafile and commit.
//
// On Windows such a request is already a no-op in osal_mresize() (it never lowers the mapping without the un-map
// permission), so there the test pins the contract without detecting the old behavior.

#include "../env-guard.h++"
#include "../test-env.h++"
#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

namespace {

const char *const kPath = "issue0592.mdbx";

/* The size of the datafile mapping as the process tracks it: the pages below it are what the writer may use. */
uint64_t mapped(const MDBX_env *env) {
  size_t current = 0;
  return mdbx_tst_env_mapping(env, nullptr, nullptr, &current) == MDBX_SUCCESS ? current : 0;
}

/* Puts the values of `key` up to `limit` until the mapping reaches `target` bytes; returns the next key. */
uint64_t grow_mapping(MDBX_env *env, MDBX_txn *txn, MDBX_dbi dbi, std::vector<char> &value, uint64_t key,
                      uint64_t limit, uint64_t target) {
  while (mapped(env) < target && key < limit) {
    MDBX_val k = {&key, sizeof(key)}, v = {value.data(), value.size()};
    if (mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT) != MDBX_SUCCESS)
      break;
    ++key;
  }
  return key;
}

} // namespace

TEST(issue_gh0592, stale_grow_request_keeps_the_mapping) {
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  MDBX_env *env = nullptr;
  env_handle_guard env_owner(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 2));
  /* a modest upper bound (the ASan/Valgrind cells cannot map the default) and a small growth step, so the writer
   * grows the mapping by itself after a few values */
  const intptr_t growth = intptr_t(64) << 10;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, 0, 0, intptr_t(64) << 20, growth, -1, -1));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kPath, MDBX_NOSUBDIR | MDBX_WRITEMAP, 0664));

  MDBX_envinfo info;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
  const uint64_t pagesize = info.mi_dxb_pagesize;
  /* a value per page, so the element size, not the count, absorbs the page size */
  std::vector<char> value(size_t(pagesize / 2), 'x');
  const uint64_t limit = (uint64_t(64) << 20) / pagesize;

  MDBX_txn *txn = nullptr;
  txn_handle_guard txn_owner(&txn);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, "values", MDBX_CREATE | MDBX_INTEGERKEY, &dbi));

  /* the snapshot a reader would have taken before the writer grew the datafile */
  const uint64_t stale_used = mdbx_tst_geo_first_unallocated(txn);
  const uint64_t stale_mapped = mapped(env);
  ASSERT_GT(stale_mapped, 0u);
  const uint64_t stale_size = stale_mapped / pagesize;
  ASSERT_LE(stale_used, stale_size);

  uint64_t key = grow_mapping(env, txn, dbi, value, 0, limit, stale_mapped + 2 * uint64_t(growth));
  const uint64_t grown = mapped(env);
  ASSERT_GE(grown, stale_mapped + 2 * uint64_t(growth)) << "the writer did not grow the mapping";

  /* the reader's request, decided before the growth, arrives now */
  int rc = MDBX_PANIC;
  std::thread reader([&] { rc = mdbx_tst_dxb_grow(env, stale_used, stale_size); });
  reader.join();
  EXPECT_EQ(MDBX_SUCCESS, rc);
  const uint64_t after = mapped(env);
  ASSERT_GE(after, grown) << "a stale grow request reduced the mapping under the write transaction: " << grown << " -> "
                          << after << " bytes, requested " << stale_size << " pages of " << pagesize << " bytes";

  /* the writer goes on allocating at the end of the datafile */
  const uint64_t written = grow_mapping(env, txn, dbi, value, key, limit, grown + 2 * uint64_t(growth));
  ASSERT_GT(written, key);
  key = written;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  txn = nullptr;

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn));
  MDBX_stat stat;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_stat(txn, dbi, &stat, sizeof(stat)));
  EXPECT_EQ(key, stat.ms_entries);
}
