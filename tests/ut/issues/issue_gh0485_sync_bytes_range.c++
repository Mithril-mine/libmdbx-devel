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

/* Regression for the defect V-055 of the verification campaign
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/485):
 * mdbx_env_set_option(MDBX_opt_sync_bytes) validated the byte value only
 * against SIZE_MAX and stored bytes2pgno() of it into the uint32
 * lck->autosync_threshold atom. Since bytes2pgno() itself returns pgno_t
 * (uint32_t), any value whose page count exceeds UINT32_MAX was silently
 * truncated: on a 4 KiB-page environment setting 2^44 stored 0, i.e. the
 * requested multi-terabyte autosync threshold silently became "sync at
 * every write", and the get/set round trip diverged. */

TEST(issue_gh0485, sync_bytes_over_range_is_rejected) {
  const env_files_guard guard({"issue485_syncbytes.mdbx"});

  MDBX_env *env = nullptr;
  env_handle_guard henv(&env);
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_maxdbs(env, 2));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, "issue485_syncbytes.mdbx", MDBX_NOSUBDIR, 0644));

  /* the page size drives the representable range: the threshold is a page
   * count in a uint32 atom, so the highest accepted value is
   * (2^32 - 1) pages */
  MDBX_envinfo info;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
  ASSERT_GT(info.mi_dxb_pagesize, 0u);
  const uint64_t pagesize = info.mi_dxb_pagesize;
  const uint64_t last_fit = ((uint64_t)UINT32_MAX) * pagesize;       /* (2^32-1) pages */
  const uint64_t first_over = ((uint64_t)UINT32_MAX + 1) * pagesize; /* 2^32 pages */

  /* a working value must be accepted and round-trip through the page
   * rounding (1 MiB is a whole number of pages for any page size) */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_sync_bytes, 1u << 20));
  uint64_t value = 0;
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_get_option(env, MDBX_opt_sync_bytes, &value));
  EXPECT_EQ(1u << 20, value);

  /* the last value whose page count still fits the uint32 atom is the
   * highest legal one (only the page rounding may differ); on hosts with
   * a 32-bit size_t it is beyond the pre-existing SIZE_MAX guard and is
   * refused by it, so the exact-fit acceptance is checked only where the
   * value is representable */
  const bool last_fit_representable = last_fit <= (uint64_t)SIZE_MAX - 65536;
  if (last_fit_representable) {
    EXPECT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_sync_bytes, last_fit));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_get_option(env, MDBX_opt_sync_bytes, &value));
    EXPECT_EQ(last_fit, value);
  }

  /* one page more does not fit and must be rejected with MDBX_EINVAL
   * instead of being silently truncated (was: accepted, get returned 0
   * on a 4 KiB-page environment). On hosts with a 32-bit size_t the
   * pre-existing SIZE_MAX guard rejects even more - the exact code is
   * not asserted there, only the rejection itself. */
  const uint64_t surviving = last_fit_representable ? last_fit : (uint64_t)1 << 20;
  EXPECT_EQ(MDBX_EINVAL, mdbx_env_set_option(env, MDBX_opt_sync_bytes, first_over))
      << "an over-range threshold must be rejected, not silently truncated";

  /* the previously set value must survive the rejected request */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_get_option(env, MDBX_opt_sync_bytes, &value));
  EXPECT_EQ(surviving, value);

  /* the default sentinel keeps working and reports the default */
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_option(env, MDBX_opt_sync_bytes, UINT64_MAX));
  ASSERT_EQ(MDBX_SUCCESS, mdbx_env_get_option(env, MDBX_opt_sync_bytes, &value));
  EXPECT_GT(value, 0u);
  EXPECT_LE(value, last_fit);
}
