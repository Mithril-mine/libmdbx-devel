/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

#include "mdbx.h"
#include <gtest/gtest.h>

#include "../env-guard.h++"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

/* Regression for the defect pagesize-discovery-damaged-meta-blindspot of the verification campaign
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/485):
 * dxb_read_header() probes meta-1/meta-2 at multiples of the system page
 * size until a meta latches the real one. With a damaged meta-0 and a
 * database page size other than the system one, every such offset landed
 * mid-page, no page size was ever latched, and the open failed with
 * MDBX_CORRUPTED despite two intact meta pages. */

static uint32_t detect_system_pagesize() {
  MDBX_env *probe = nullptr;
  if (mdbx_env_create(&probe) != MDBX_SUCCESS)
    return 0;
  /* for a not-open environment mi_dxb_pagesize reports the initial
   * page-size guess, i.e. the (clamped) system page size */
  MDBX_envinfo info;
  std::memset(&info, 0, sizeof(info));
  const int rc = mdbx_env_info_ex(probe, nullptr, &info, sizeof(info));
  mdbx_env_close(probe);
  return (rc == MDBX_SUCCESS) ? info.mi_dxb_pagesize : 0;
}

static void tear_meta0(const char *path) {
  /* zero the beginning of the file: the meta-0 page header (pgno, flags)
   * and the magic_and_version field of the meta live in the first bytes */
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.good());
  const char zeros[48] = {0};
  file.seekp(0);
  file.write(zeros, sizeof(zeros));
  ASSERT_TRUE(file.good());
}

TEST(issue_gh0485, open_via_intact_metas_with_damaged_meta0) {
  const uint32_t sys_pagesize = detect_system_pagesize();
  ASSERT_GE(sys_pagesize, MDBX_MIN_PAGESIZE);
  /* the blind spot needs a database page size different from the system
   * one; pick one of the two candidates that always differs from it */
  const uint32_t pagesize = (sys_pagesize != 256) ? 256 : 16384;

  const env_files_guard guard({"issue485_meta0.mdbx"});

  MDBX_env *env = nullptr;
  {
    env_handle_guard henv(&env);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, 0, 0, 0, -1, -1, (int)pagesize));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, "issue485_meta0.mdbx", MDBX_NOSUBDIR, 0644));

    MDBX_txn *txn = nullptr;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi = 0;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_CREATE, &dbi));
    const char key_data[] = "k", value_data[] = "v";
    MDBX_val key = {const_cast<char *>(key_data), 1};
    MDBX_val value = {const_cast<char *>(value_data), 1};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &value, MDBX_UPSERT));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
    mdbx_env_close(env);
    env = nullptr;
  }

  tear_meta0("issue485_meta0.mdbx");

  /* meta-1/meta-2 are intact: the open must succeed through them
   * (was MDBX_CORRUPTED when the page size differs from the system one) */
  env = nullptr;
  env_handle_guard henv(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  const int rc = mdbx_env_open(env, "issue485_meta0.mdbx", MDBX_NOSUBDIR | MDBX_RDONLY, 0644);
  ASSERT_EQ(MDBX_SUCCESS, rc) << "two intact meta pages must survive a damaged meta-0";
  if (rc == MDBX_SUCCESS) {
    MDBX_envinfo info;
    std::memset(&info, 0, sizeof(info));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
    EXPECT_EQ(pagesize, info.mi_dxb_pagesize) << "the real page size must be discovered";
  }
}
