/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not limited to
/// the warranties of merchantability, fitness for a particular purpose and non infringement. In no event shall the
/// authors or copyright holders be liable for any claim, damages or other liability, whether in an action of contract,
/// tort or otherwise, arising from, out of or in connection with the software or the use or other dealings in the
/// software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/619
//
// The GC keeps the list of its free pages in a large page. When the list is long — more than about fourteen pages of
// the smallest page size — there is no run of free pages of that length in a table of one-page records, so the GC
// stores the list right above the last data page, and the pages the cycle retired when it loaded the GC stay there.
// The walk of the defragmentation requires the page it moves to lie right below the edge of the walk; a retired page
// between them made it stop with "the adjacent page is in the retired list", and the next cycle met the same obstacle:
// nothing moved, and the call ended with MDBX_defrag_error although 1051 of its 2153 pages were free.
//
// A page above the data that is retired by the transaction, or reclaimed but not the lowest of the free ones, does not
// block the pages below: the commit stores the GC list anew, and the free pages above are cut off with the tail. So the
// walk goes on, the pages move into the free slots, and the datafile shrinks.

#include "../env-guard.h++"
#include <gtest/gtest.h>

#include "../test-env.h++"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

const char *const kPath = "issue0619.mdbx";
const char *const kTable = "t0";

/* The table of the issue: one record to a page, every second one deleted. The number of the free pages, and so the
 * length of the list the GC keeps, does not fit in a run of the free pages found, and the list is stored above the data
 * when there are enough of them: at the smallest page sizes about fourteen pages of the list and more. */
struct layout {
  size_t pgsize;
  unsigned records;
};

/* The environment of the issue: the smallest pages, the modes of the test and no sync at all. */
MDBX_env *open_environment(const layout &l) {
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return nullptr;
  mdbx_env_set_maxdbs(env, 8);
  mdbx_env_set_maxreaders(env, 8);
  /* The sequences need a few megabytes. The upper limit is kept far below the limit of the mapping, which is small
   * under the sanitizers. */
  const intptr_t upper = static_cast<intptr_t>(8192) * static_cast<intptr_t>(l.pgsize);
  const MDBX_env_flags_t mode = MDBX_UTTERLY_NOSYNC | MDBX_NOSUBDIR | MDBX_NOSTICKYTHREADS | MDBX_LIFORECLAIM;
  if (mdbx_env_set_geometry(env, -1, -1, upper, -1, -1, l.pgsize) != MDBX_SUCCESS ||
      mdbx_env_open(env, kPath, mode, 0644) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return nullptr;
  }
  return env;
}

/* The table of the test: a record of half a page to a page for every odd number, the even ones deleted. */
bool fill_table(MDBX_env *env, MDBX_dbi *dbi, const layout &l) {
  MDBX_txn *txn = nullptr;
  txn_handle_guard owner(&txn);
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    return false;
  if (mdbx_dbi_open(txn, kTable, MDBX_CREATE, dbi) != MDBX_SUCCESS)
    return false;
  const std::string value(l.pgsize / 2, 'v');
  for (unsigned i = 0; i < l.records; ++i) {
    MDBX_val k = {&i, sizeof(i)}, v = {const_cast<char *>(value.data()), value.size()};
    if (mdbx_put(txn, *dbi, &k, &v, MDBX_UPSERT) != MDBX_SUCCESS)
      return false;
  }
  for (unsigned i = 0; i < l.records; i += 2) {
    MDBX_val k = {&i, sizeof(i)};
    if (mdbx_del(txn, *dbi, &k, nullptr) != MDBX_SUCCESS)
      return false;
  }
  owner.release();
  return mdbx_txn_commit(txn) == MDBX_SUCCESS;
}

/* Every record left is read back and compared with what was put. */
bool verify_table(MDBX_env *env, MDBX_dbi dbi, const layout &l) {
  const std::string value(l.pgsize / 2, 'v');
  MDBX_txn *txn = nullptr;
  txn_handle_guard owner(&txn);
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_RDONLY, &txn) != MDBX_SUCCESS)
    return false;
  for (unsigned i = 1; i < l.records; i += 2) {
    MDBX_val k = {&i, sizeof(i)}, v;
    if (mdbx_get(txn, dbi, const_cast<MDBX_val *>(&k), &v) != MDBX_SUCCESS)
      return false;
    if (v.iov_len != value.size() || memcmp(v.iov_base, value.data(), value.size()) != 0)
      return false;
  }
  return true;
}

void run_case(const layout &l) {
  if (l.pgsize < static_cast<size_t>(mdbx_limits_pgsize_min())) {
    GTEST_SKIP() << "the page size " << l.pgsize << " is below the smallest of the platform";
    return;
  }
  const env_files_guard guard({kPath});
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  MDBX_env *env = open_environment(l);
  env_handle_guard env_owner(&env);
  ASSERT_NE(nullptr, env) << "a page size of " << l.pgsize;
  MDBX_dbi dbi;
  ASSERT_TRUE(fill_table(env, &dbi, l)) << "a page size of " << l.pgsize;

  MDBX_defrag_result_t result;
  memset(&result, 0, sizeof(result));
  const int rc = mdbx_env_defrag(env, 0, 0, 0, 0, -1, 0, nullptr, nullptr, &result);
  EXPECT_EQ(MDBX_SUCCESS, rc) << " pages of " << l.pgsize << " bytes, stopping_reasons=" << result.stopping_reasons;
  EXPECT_EQ(0u, result.stopping_reasons & MDBX_defrag_error) << " pages of " << l.pgsize << " bytes";
  EXPECT_GT(result.pages_moved, 0u) << " pages of " << l.pgsize << " bytes: the pages moved into the free slots";
  EXPECT_GT(result.pages_shrinked, intptr_t(0)) << " pages of " << l.pgsize << " bytes: the datafile became smaller";
  EXPECT_TRUE(verify_table(env, dbi, l)) << " pages of " << l.pgsize << " bytes";

  /* nothing is left to move: a repeated call moves no page and reports no error */
  MDBX_defrag_result_t again;
  memset(&again, 0, sizeof(again));
  const int again_rc = mdbx_env_defrag(env, 0, 0, 0, 0, -1, 0, nullptr, nullptr, &again);
  EXPECT_TRUE(again_rc == MDBX_SUCCESS || again_rc == MDBX_RESULT_TRUE) << "rc " << again_rc;
  EXPECT_EQ(0u, again.stopping_reasons & MDBX_defrag_error);
  EXPECT_EQ(0u, again.pages_moved) << "the first defragmentation left nothing to move";
}

} // namespace

/* A table of one-page records, every second one deleted: the list of the free pages of the GC is long, and the pages it
 * occupies above the data must not block the walk. */
TEST(issue_gh0619, the_gc_free_list_does_not_block_the_walk) {
  /* The rows of the issue: 1800 records give 900 free pages and a list of 15 pages at 256 bytes, which is more than a
   * run of the free pages found in the table; 3000 records do the same with a longer list. */
  run_case({256, 1800});
  run_case({256, 3000});
  run_case({512, 6000});
}
