/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/* Regression for the RW-exclusive leg of the defect geometry-from-db-32bit-wrap
 * (https://github.com/Mithril-mine/libmdbx-devel/issues/636): the #485 fix formed
 * the geometry-from-db byte values in 64 bits and refused the over-envelope
 * geometry only in the first branch of dxb_setup() (readonly / not-exclusive /
 * recovery), while the RW-exclusive open with no user-preconfigured geometry
 * takes the third branch, where the grow step was still formed through
 * pgno_ceil2os_bytes() in size_t. A crafted meta whose grow_pv encodes EXACTLY
 * 65536 pages (pages2pv(65536) == 0x80FF) at a 65536-byte page size carries the
 * byte value exactly 2^32: on a 32-bit host the unfixed code silently wrapped
 * it to zero (mi_geo.grow == 0, growth disabled) and opened the database; the
 * fixed code refuses it cleanly with MDBX_INCOMPATIBLE, the same way the first
 * branch does.
 *
 * Unlike crafted_meta_v023 (the historical quantized 0x8101 masking case) this
 * unit runs on every host and covers both legs (tests/AGENTS.md §11): on a
 * 64-bit host it asserts the exact preserved grow step (2^32 bytes), on a
 * 32-bit host - the clean refusal. The android armeabi-v7a and win32 MSVC CI
 * cells are 32-bit and exercise the refusal leg. The crafted meta page is
 * built through the testing API's layout accessors (tests/AGENTS.md §8, issue
 * #519 stage 5); the grow_pv is taken through the live mdbx_tst_pages2pv()
 * encoder, unlike the historical hand-picked 0x8101 of crafted_meta_v023
 * which quantizes away from the exact 65536 pages.
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

constexpr const char kDbPath[] = "crafted-v636.mdbx";
constexpr uint64_t kGrowBytesExact = 4294967296u; /* 65536 pages x 65536 bytes = 2^32 */

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

TEST(crafted_meta_v636, rw_exclusive_grow_step_exact_2_32) {
  env_files_guard guard({kDbPath});
  /* the live encoder, unlike the historical hand-picked 0x8101 of
   * crafted_meta_v023 which quantizes away from the exact 65536 pages */
  const uint16_t crafted_grow_pv = mdbx_tst_pages2pv(65536);
  EXPECT_NE(0x8101, crafted_grow_pv)
      << "pages2pv(65536) quantized to the 65792-page 0x8101, the exact 2^32 case is not exercised";
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

  /* craft the winner meta slot only: small in-range lower/now/upper/
   * first_unallocated plus the exactly-2^32-bytes grow_pv; the txnid is
   * bumped so the crafted slot wins the latch; re-signed so the checksum
   * branch passes and the geometry conversion decides the verdict */
  memcpy(work, baseline, ps);
  mdbx_tst_page_set_pgno(work, baseline_pgno);
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_LOWER, 3);
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_UPPER, 1000); /* x pagesize fits the 32-bit intptr_t */
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_NOW, 4);
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_FIRST_UNALLOCATED, 4);
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

  if (sizeof(size_t) > 4) {
    /* a 64-bit host: the whole geometry fits the address space, the exact
     * 2^32 grow step must be preserved, not quantized or clamped */
    env_handle_guard henv(&env);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644));
    MDBX_envinfo info;
    memset(&info, 0, sizeof(info));
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_info_ex(env, nullptr, &info, sizeof(info)));
    EXPECT_EQ(kGrowBytesExact, info.mi_geo.grow) << "the exact 2^32 grow step must be preserved on a 64-bit host";
  } else {
    /* a 32-bit host: the fixed code refuses the database cleanly; the
     * unfixed code opened it with mi_geo.grow wrapped to zero (growth
     * disabled) */
    env_handle_guard henv(&env);
    ASSERT_EQ(MDBX_SUCCESS, mdbx_env_create(&env));
    const int rc = mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644);
    if (rc == MDBX_SUCCESS) {
      FAIL() << "the exactly-2^32 grow step silently wrapped to zero (growth disabled) and the database opened";
      return;
    }
    EXPECT_EQ(MDBX_INCOMPATIBLE, rc) << "the over-envelope geometry must be refused cleanly, not with some other error";
  }
}
