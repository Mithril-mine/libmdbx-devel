/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/* Regression for the defect geometry-from-db-32bit-wrap of the verification campaign
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/485):
 * the geometry-from-db byte values (page-count x pagesize) were formed in
 * size_t and wrap modulo 2^32 on 32-bit hosts, because meta_validate()
 * bounds the page counts only by MAX_PAGENO. A crafted meta whose grow_pv
 * encodes 65792 pages at a 65536-byte page size carries the byte value
 * 4.01GB: on a 32-bit host the geometry-from-db byte arithmetic overflows
 * the 32-bit size_t, and the test requires the reopen to keep the growth
 * usable (a non-zero mi_geo.grow) instead of silently disabling it.
 *
 * The test runs only where the defect is expressible: on a 32-bit host
 * (a runtime sizeof gate, no preprocessor cut). The android armeabi-v7a and
 * win32 MSVC CI cells are 32-bit and exercise it; on 64-bit hosts it skips.
 * The crafted meta page is built through the testing API's layout accessors
 * (tests/AGENTS.md §8, issue #519 stage 5): the field crafting and the
 * on-disk-format knowledge stay inside the testing API.
 *
 * The crafted meta is written to the winner slot only, with the txnid
 * bumped so the slot wins the troika latch (writing the same meta to all
 * three slots would clash the troika). */

#include "../env-guard.h++"
#include <gtest/gtest.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The constant word-size conditional of the 32-bit gate below trips the
 * MSVC C4127 under /W4 /WX - suppressed inside the library by
 * src/preface.h, and mirrored here for this standalone unit. */
#if defined(_MSC_VER)
#pragma warning(disable : 4127)
#endif /* _MSC_VER */

/*-----------------------------------------------------------------------------*/

namespace {

constexpr const char kDbPath[] = "crafted-v023.mdbx";

/* The union grants the 8-byte alignment (a plain static uint8_t array may
 * sit at any byte address), which the unaligned u64 field access requires. */
union align8_u {
  uint64_t force_align;
  uint8_t bytes[MDBX_MAX_PAGESIZE];
};
align8_u baseline_buf, work_buf;
uint8_t *const baseline = baseline_buf.bytes;
uint8_t *const work = work_buf.bytes;

} /* namespace */

TEST(crafted_meta_v023, geometry_from_db_32bit_wrap) {
  env_files_guard guard({kDbPath});
  if (sizeof(size_t) > 4) {
    GTEST_SKIP() << "SKIPPED on a " << sizeof(void *) * 8 << "-bit host (the wrap needs a 32-bit size_t)";
    return;
  }
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }

  /* a real database at the 65536-byte page size, with an explicit generous
   * geometry (the platform defaults at this page size are tight) */
  MDBX_env *env = nullptr;
  {
    env_handle_guard henv(&env);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_set_geometry(env, 0, 32 * 1024 * 1024, 512 * 1024 * 1024, 0, 0, 65536));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644));
    MDBX_txn *txn = nullptr;
    txn_handle_guard htxn(&txn);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn));
    MDBX_dbi dbi;
    ASSERT_EQ(MDBX_SUCCESS, mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi));
    MDBX_val key = {(void *)"k", 1}, value = {(void *)"v", 1};
    ASSERT_EQ(MDBX_SUCCESS, mdbx_put(txn, dbi, &key, &value, (MDBX_put_flags_t)0));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_txn_commit(txn));
    htxn.release();
  }

  /* snapshot a valid meta page (the corpus pattern) */
  unsigned baseline_pgno = MDBX_TST_NUM_METAS;
  size_t ps = 0;
  bool found = false;
  {
    env_handle_guard henv(&env);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR | MDBX_RDONLY, 0444));
    ps = mdbx_tst_env_pagesize(env);
    for (unsigned n = 0; n < MDBX_TST_NUM_METAS && !found; ++n) {
      if (mdbx_tst_env_read_page(env, n, baseline, ps) != MDBX_SUCCESS)
        break;
      if (mdbx_tst_meta_validate(env, baseline, n, nullptr) == MDBX_SUCCESS) {
        found = true;
        baseline_pgno = n;
      }
    }
  }
  ASSERT_TRUE(found) << "no valid meta page in the fresh database";

  /* craft the winner meta slot only (writing the same meta to all three
   * slots would clash the troika): small in-range lower/now/upper/
   * first_unallocated plus the over-range grow_pv; the txnid is bumped so
   * the crafted slot wins the latch; re-signed so the checksum branch
   * passes and the geometry conversion decides the verdict */
  memcpy(work, baseline, ps);
  mdbx_tst_page_set_pgno(work, baseline_pgno);
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_LOWER, 3);
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_UPPER, 1000); /* x pagesize fits the 32-bit intptr_t */
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_NOW, 4);
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_FIRST_UNALLOCATED, 4);
  /* the grow step just above the 32-bit byte envelope: the next pv-encoded
   * value above 65536 pages (the pv quantization at the exponent zero goes
   * in 256-page increments, so 65536 + 256 = 65792 pages x 65536 bytes =
   * 4.01GB). Taken through the live encoder - pages2pv(65792) is the same
   * 0x8101 the pre-rework test hard-coded, so an encoding drift is
   * impossible; note pv2pages(0x8101) is 65792 pages, not 65536. */
  const uint16_t crafted_grow_pv = mdbx_tst_pages2pv(65792);
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_GROW_PV, crafted_grow_pv);
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_SHRINK_PV, 0);
  const uint64_t next_txnid = mdbx_tst_meta_field_get(work, MDBX_TST_META_TXNID_A) + 1;
  mdbx_tst_meta_field_set(work, MDBX_TST_META_TXNID_A, next_txnid);
  mdbx_tst_meta_field_set(work, MDBX_TST_META_TXNID_B, next_txnid);
  mdbx_tst_meta_sign_as_steady(work);

  /* the file I/O at the slot's offset: meta slot n lives at n x pagesize */
  {
    FILE *f = fopen(kDbPath, "r+b");
    ASSERT_NE(nullptr, f);
    fseek(f, (long)baseline_pgno * (long)ps, SEEK_SET);
    ASSERT_EQ(ps, fwrite(work, 1, ps, f));
    fclose(f);
  }

  /* the reopen must keep the growth usable: the crafted over-range grow
   * step is clamped (geometry-from-db-32bit-wrap) instead of silently wrapping to zero */
  {
    env_handle_guard henv(&env);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644));
    MDBX_envinfo info;
    memset(&info, 0, sizeof(info));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
    EXPECT_NE(0, info.mi_geo.grow) << "the 2^32-byte grow step silently wrapped to zero (growth disabled)";
  }
}
