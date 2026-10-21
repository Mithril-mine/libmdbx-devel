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

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../test-env.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _MSC_VER
#pragma warning(disable : 4702) /* unreachable code */
#endif

// Regression test for https://github.com/Mithril-mine/libmdbx-devel/issues/117
// 1. writemap_dirty_npages and writemap_spilled_npages were the same union
//    cell, so with MDBX_WRITEMAP mdbx_txn_info() reported twice the dirty
//    space before a spill and lost the spilled pages after it.
// 2. mdbx_ratio2digits() with precision <= 0 returned its result with one
//    uninitialized trailing byte.

TEST(issue_gh0117, writemap_dirty_space_is_not_doubled) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  const char *const testdb = "issue117.mdbx";
  remove(testdb);

  MDBX_env *env = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_max_db, 4));
  /* Pin 4-KiB database pages so each 3072-byte value lives on its own
   * overflow page regardless of the platform default; otherwise on large
   * pages the values fit inline and the doubled dirty count stays below any
   * page-count bound. */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, -1, -1, -1, -1, -1, 4096));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, testdb, MDBX_NOSUBDIR | MDBX_WRITEMAP, 0644));

  MDBX_txn *txn = nullptr;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
  MDBX_dbi dbi = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));

  /* several puts of 3 KB values: enough dirty pages to be visible in the info */
  for (unsigned i = 0; i < 200; ++i) {
    char kb[16], vb[3072];
    snprintf(kb, sizeof(kb), "k%u", i);
    memset(vb, 'a' + (i % 26), sizeof(vb));
    MDBX_val k = {kb, strlen(kb)}, v = {vb, sizeof(vb)};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &k, &v, MDBX_UPSERT));
  }

  MDBX_txn_info info;
  memset(&info, 0, sizeof(info));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_info(txn, &info, false));
  EXPECT_GT(info.txn_space_dirty, (uint64_t)0) << "the dirty space must be reported";
  /* 200 values of ~3 KB each live on their own overflow pages: the correct
   * dirty byte count is about 200 pages, while the doubled one (the aliasing
   * bug reported) is about 400. A 300-page bound separates the two without
   * depending on the platform page size. */
  MDBX_envinfo einfo;
  memset(&einfo, 0, sizeof(einfo));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &einfo, sizeof(einfo)));
  const size_t pagesize = einfo.mi_dxb_pagesize;
  EXPECT_LT(info.txn_space_dirty, (uint64_t)(300 * pagesize))
      << "the writemap dirty byte count must not be doubled by counter aliasing";

  ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
  mdbx_env_close(env);
  remove(testdb);
}

TEST(issue_gh0117, ratio2digits_zero_precision_has_no_stray_byte) {
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  char buf[64];
  memset(buf, 'Z', sizeof(buf));
  const char *const s = mdbx_ratio2digits(5, 1, 0, buf, sizeof(buf));
  EXPECT_STREQ("5", s) << "precision 0 must format the integer part only";
  EXPECT_EQ('5', s[0]);
  EXPECT_EQ('\0', s[1]) << "the string must be terminated right after the digits";
}