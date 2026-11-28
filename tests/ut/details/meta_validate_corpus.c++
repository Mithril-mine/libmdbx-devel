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

/* A corpus of crafted meta pages, one case per reject branch of
 * meta_validate() (src/meta.c): every field mutation below must flip exactly
 * the intended branch and produce the documented return code. The baseline is
 * a real committed meta page snapshotted from a small database, so the
 * positive control (the pristine copy validates as MDBX_SUCCESS) holds on
 * every page size / geometry combination. Every access goes through the
 * testing API (tests/AGENTS.md §8, issue #519 stage 5): the validation runs
 * live through mdbx_tst_meta_validate(), and the page/meta field crafting is
 * the layout-accessor family - no on-disk format knowledge lives here.
 *
 * The meta_validate reject-branch corpus (the reject branches enumerated
 * by the 2026-11 verification campaign, issue #485): magic/version, page-pgno, page-flags, pagesize, txnid_a !=
 * txnid_b, steady-sign, gc-flags, main-flags, txnid range, geo.lower, geo.upper, first_unallocated,
 * allocated-vs-filesize, the mmap_limit bound (made always-reachable through the test-only
 * mdbx_tst_override_mmap_limit), the #454 height/root/leaf/branch equivalences, false-empty trees and mod_txnid.
 *
 * NOTE: format-dependent by purpose (the corpus walks the on-disk format's
 * reject branches); a format change must revisit the accessor selectors'
 * expectations, the field set itself is pinned by the testing API. */

#include "../env-guard.h++"
#include <gtest/gtest.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*-----------------------------------------------------------------------------*/

namespace {

constexpr const char kDbPath[] = "meta-validate-corpus.mdbx";

/* The union grants the 8-byte alignment (a plain static uint8_t array may
 * sit at any byte address), which the unaligned u64 field access requires. */
union align8_u {
  uint64_t force_align;
  uint8_t bytes[MDBX_MAX_PAGESIZE];
};
align8_u baseline_buf, work_buf;
uint8_t *const baseline = baseline_buf.bytes;
uint8_t *const work = work_buf.bytes;
unsigned g_ps = 0;
uint32_t file_pages = 0;
unsigned baseline_pgno = 0;
MDBX_env *g_env = nullptr;

/* expect_rc() mirrors the engine's verdict vocabulary: the corpus cases
 * continue after a mismatch, so a single run reports every deviation. */
void expect_rc(const char *name, int expected, int got) { EXPECT_EQ(expected, got) << name; }

/* A fresh byte copy of the baseline page: the page header and the meta
 * payload overlay the same buffer, exactly like a mapped meta page. */
void copy_page(void) { memcpy(work, baseline, g_ps); }

/* Validates the current (already mutated and re-signed) work buffer as the
 * meta slot it claims to be (the baseline slot number). */
void validate(const char *name, int expected) {
  expect_rc(name, expected, mdbx_tst_meta_validate(g_env, work, baseline_pgno, nullptr));
}

/* Builds a small database, opens it read-only and snapshots a valid meta
 * page as the corpus baseline: at least the troika winner of the committed
 * database must validate successfully. */
bool setup_corpus(void) {
  remove(kDbPath);
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    return false;
  if (mdbx_env_set_geometry(env, 0, 0, 0, 0, 0, 0) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return false;
  }
  if (mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return false;
  }
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return false;
  }
  MDBX_dbi dbi;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return false;
  }
  MDBX_val key = {(void *)"k", 1}, value = {(void *)"v", 1};
  if (mdbx_put(txn, dbi, &key, &value, (MDBX_put_flags_t)0) != MDBX_SUCCESS) {
    mdbx_env_close(env);
    return false;
  }
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
    return false;
  if (mdbx_env_close(env) != MDBX_SUCCESS)
    return false;

  if (mdbx_env_create(&g_env) != MDBX_SUCCESS)
    return false;
  if (mdbx_env_open(g_env, kDbPath, MDBX_NOSUBDIR | MDBX_RDONLY, 0644) != MDBX_SUCCESS)
    return false;
  g_ps = mdbx_tst_env_pagesize(g_env);
  if (g_ps > MDBX_MAX_PAGESIZE)
    return false;
  file_pages = (uint32_t)(mdbx_tst_env_filesize(g_env) / g_ps);

  bool found = false;
  for (unsigned n = 0; n < MDBX_TST_NUM_METAS && !found; ++n) {
    if (mdbx_tst_env_read_page(g_env, n, work, g_ps) != MDBX_SUCCESS)
      return false;
    if (mdbx_tst_meta_validate(g_env, work, n, nullptr) == MDBX_SUCCESS) {
      memcpy(baseline, work, g_ps);
      found = true;
    }
  }
  if (!found) {
    ADD_FAILURE() << "no valid meta page in the fresh database";
    return false;
  }
  baseline_pgno = mdbx_tst_page_get_pgno(baseline);
  return true;
}

/* Every case below mutates exactly one aspect of the otherwise-valid copy;
 * the sign is refreshed (except for the sign case itself) so the checksum
 * branch passes and the branch under test decides the verdict. */

} /* namespace */

TEST(meta_validate_corpus, pristine_baseline) {
  env_files_guard guard({kDbPath});
  env_handle_guard henv(&g_env);
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  ASSERT_TRUE(setup_corpus());
  validate("pristine-baseline", MDBX_SUCCESS);
}

TEST(meta_validate_corpus, magic_version) {
  env_files_guard guard({kDbPath});
  env_handle_guard henv(&g_env);
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  ASSERT_TRUE(setup_corpus());
  {
    copy_page();
    /* corrupt two bytes of the magic-and-version word: the expectation is
     * derived from the corrupted word, so the case is endian-agnostic */
    uint64_t v = mdbx_tst_meta_field_get(work, MDBX_TST_META_MAGIC_AND_VERSION);
    v ^= UINT64_C(0x5a);
    v ^= UINT64_C(0xa5) << 8;
    mdbx_tst_meta_field_set(work, MDBX_TST_META_MAGIC_AND_VERSION, v);
    v = mdbx_tst_meta_field_get(work, MDBX_TST_META_MAGIC_AND_VERSION);
    expect_rc("bad-magic", (v >> 8) != MDBX_TST_MAGIC ? MDBX_INVALID : MDBX_VERSION_MISMATCH,
              mdbx_tst_meta_validate(g_env, work, baseline_pgno, nullptr));
  }
  {
    /* the magic word intact but an unknown data version: the version is the
     * low byte of the 64-bit magic-and-version word */
    copy_page();
    const uint64_t v = mdbx_tst_meta_field_get(work, MDBX_TST_META_MAGIC_AND_VERSION);
    mdbx_tst_meta_field_set(work, MDBX_TST_META_MAGIC_AND_VERSION, (v & ~(uint64_t)0xFF) | 42);
    expect_rc("bad-version", MDBX_VERSION_MISMATCH, mdbx_tst_meta_validate(g_env, work, baseline_pgno, nullptr));
  }
}

TEST(meta_validate_corpus, page_header) {
  env_files_guard guard({kDbPath});
  env_handle_guard henv(&g_env);
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  ASSERT_TRUE(setup_corpus());
  {
    copy_page();
    mdbx_tst_meta_sign_as_steady(work);
    mdbx_tst_page_set_pgno(work, mdbx_tst_page_get_pgno(work) + 1);
    validate("wrong-page-pgno", MDBX_INVALID);
  }
  {
    copy_page();
    mdbx_tst_meta_sign_as_steady(work);
    mdbx_tst_page_set_flags(work, MDBX_TST_PAGE_FLAG_LEAF);
    validate("wrong-page-flags", MDBX_INVALID);
  }
}

TEST(meta_validate_corpus, pagesize) {
  env_files_guard guard({kDbPath});
  env_handle_guard henv(&g_env);
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  ASSERT_TRUE(setup_corpus());
  {
    copy_page();
    mdbx_tst_meta_pagesize_set(work, 1000 /* deliberately not a power of 2 */);
    mdbx_tst_meta_sign_as_steady(work);
    validate("pagesize-not-pow2", MDBX_INVALID);
  }
  {
    copy_page();
    mdbx_tst_meta_pagesize_set(work, MDBX_MIN_PAGESIZE / 2);
    mdbx_tst_meta_sign_as_steady(work);
    validate("pagesize-too-small", MDBX_VERSION_MISMATCH);
  }
}

TEST(meta_validate_corpus, txnid_and_sign) {
  env_files_guard guard({kDbPath});
  env_handle_guard henv(&g_env);
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  ASSERT_TRUE(setup_corpus());
  {
    copy_page();
    mdbx_tst_meta_field_set(work, MDBX_TST_META_TXNID_A, mdbx_tst_meta_field_get(work, MDBX_TST_META_TXNID_B) + 1);
    validate("txnid-a-ne-b", MDBX_RESULT_TRUE);
  }
  {
    copy_page();
    /* the baseline is steady-signed, so the stored sign is the calculated
     * one; a single-bit flip off it must trip the sign branch */
    const uint64_t good = mdbx_tst_meta_field_get(work, MDBX_TST_META_SIGN);
    const uint64_t bad = (good > 2) ? (good ^ 1) : (good + 2);
    mdbx_tst_meta_field_set(work, MDBX_TST_META_SIGN, bad);
    validate("steady-sign-mismatch", MDBX_RESULT_TRUE);
  }
  {
    copy_page();
    mdbx_tst_meta_field_set(work, MDBX_TST_META_TXNID_A, 0 /* below MIN_TXNID */);
    mdbx_tst_meta_field_set(work, MDBX_TST_META_TXNID_B, 0);
    mdbx_tst_meta_sign_as_steady(work);
    validate("txnid-out-of-range", MDBX_RESULT_TRUE);
  }
}

TEST(meta_validate_corpus, table_flags) {
  env_files_guard guard({kDbPath});
  env_handle_guard henv(&g_env);
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  ASSERT_TRUE(setup_corpus());
  {
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_GC, MDBX_TST_TREE_FLAGS, (uint16_t)MDBX_DUPSORT);
    mdbx_tst_meta_sign_as_steady(work);
    validate("gc-flags", MDBX_INCOMPATIBLE);
  }
  {
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_FLAGS, UINT16_MAX);
    mdbx_tst_meta_sign_as_steady(work);
    validate("main-flags", MDBX_INCOMPATIBLE);
  }
}

TEST(meta_validate_corpus, geometry) {
  env_files_guard guard({kDbPath});
  env_handle_guard henv(&g_env);
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  ASSERT_TRUE(setup_corpus());
  {
    copy_page();
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_LOWER, 0);
    mdbx_tst_meta_sign_as_steady(work);
    validate("geo-lower-below-min", MDBX_INVALID);
  }
  {
    copy_page();
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_LOWER, MDBX_TST_MAX_PAGENO + 2);
    mdbx_tst_meta_sign_as_steady(work);
    validate("geo-lower-above-max", MDBX_INVALID);
  }
  {
    copy_page();
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_UPPER, MDBX_TST_MAX_PAGENO + 2);
    mdbx_tst_meta_sign_as_steady(work);
    validate("geo-upper-above-max", MDBX_INVALID);
  }
  {
    copy_page();
    const uint64_t lower = mdbx_tst_meta_geo_get(work, MDBX_TST_META_GEO_LOWER);
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_UPPER,
                          (lower > MDBX_TST_MIN_PAGENO) ? lower - 1 : (uint64_t)MDBX_TST_MIN_PAGENO - 1);
    mdbx_tst_meta_sign_as_steady(work);
    validate("geo-upper-below-lower", MDBX_INVALID);
  }
  {
    copy_page();
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_FIRST_UNALLOCATED, MDBX_TST_MAX_PAGENO + 2);
    mdbx_tst_meta_sign_as_steady(work);
    validate("fua-above-max", MDBX_CORRUPTED);
  }
  {
    /* allocated space beyond the (real) file size */
    copy_page();
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_FIRST_UNALLOCATED, file_pages + 1);
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_UPPER, file_pages + 1);
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_NOW, file_pages + 1);
    mdbx_tst_meta_sign_as_steady(work);
    validate("allocated-beyond-filesize", MDBX_CORRUPTED);
  }
  {
    /* lower bound mapping above MAX_MAPSIZE64 is rejected; a bound within
     * the 64-bit envelope is silently clamped instead (the reachable side
     * depends on the platform word width, so the expectation is computed) */
    copy_page();
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_LOWER, MDBX_TST_MAX_PAGENO + 1);
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_UPPER, MDBX_TST_MAX_PAGENO + 1);
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_NOW, MDBX_TST_MAX_PAGENO + 1);
    mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_FIRST_UNALLOCATED, file_pages);
    mdbx_tst_meta_sign_as_steady(work);
    const uint64_t mapsize_min = (uint64_t)(MDBX_TST_MAX_PAGENO + 1) * mdbx_tst_meta_pagesize_get(work);
    validate("mapsize-min-bound", mapsize_min > MDBX_TST_MAX_MAPSIZE64 ? MDBX_VERSION_MISMATCH : MDBX_SUCCESS);
  }
}

TEST(meta_validate_corpus, trees) {
  env_files_guard guard({kDbPath});
  env_handle_guard henv(&g_env);
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  ASSERT_TRUE(setup_corpus());
  const uint64_t usable_root = mdbx_tst_meta_geo_get(baseline, MDBX_TST_META_GEO_FIRST_UNALLOCATED) - 1;
  if (usable_root < MDBX_TST_NUM_METAS) {
    GTEST_SKIP() << "no page beyond the meta area";
    return;
  }

  {
    /* root beyond first_unallocated */
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_ROOT,
                           mdbx_tst_meta_geo_get(work, MDBX_TST_META_GEO_FIRST_UNALLOCATED));
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_HEIGHT, 1);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_LEAF_PAGES, 1);
    mdbx_tst_meta_sign_as_steady(work);
    validate("main-root-beyond-fua", MDBX_CORRUPTED);
  }
  {
    /* root inside the meta area */
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_ROOT, 1);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_HEIGHT, 1);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_LEAF_PAGES, 1);
    mdbx_tst_meta_sign_as_steady(work);
    validate("main-root-in-metas", MDBX_CORRUPTED);
  }
  {
    /* height beyond the fixed cursor stack (the #454 family) */
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_ROOT, usable_root);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_HEIGHT, mdbx_tst_cursor_stack_size());
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_LEAF_PAGES, 1);
    mdbx_tst_meta_sign_as_steady(work);
    validate("main-height-beyond-stack", MDBX_CORRUPTED);
  }
  {
    /* height == 0 with a valid root: the #454 equivalence */
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_ROOT, usable_root);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_HEIGHT, 0);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_LEAF_PAGES, 0);
    mdbx_tst_meta_sign_as_steady(work);
    validate("main-height0-with-root", MDBX_CORRUPTED);
  }
  {
    /* height == 1 without leaf pages */
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_ROOT, usable_root);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_HEIGHT, 1);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_LEAF_PAGES, 0);
    mdbx_tst_meta_sign_as_steady(work);
    validate("main-height1-without-leaves", MDBX_CORRUPTED);
  }
  {
    /* height > 1 without branch pages */
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_ROOT, usable_root);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_HEIGHT, 2);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_LEAF_PAGES, 2);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_BRANCH_PAGES, 0);
    mdbx_tst_meta_sign_as_steady(work);
    validate("main-height2-without-branches", MDBX_CORRUPTED);
  }
  {
    /* false-empty: no root, but bookkeeping claims items */
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_ROOT, MDBX_TST_P_INVALID);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_HEIGHT, 0);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_LEAF_PAGES, 0);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_BRANCH_PAGES, 0);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_ITEMS, 42);
    mdbx_tst_meta_sign_as_steady(work);
    validate("main-false-empty", MDBX_CORRUPTED);
  }
  {
    /* the same equivalence family for the GC tree */
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_GC, MDBX_TST_TREE_ROOT, usable_root);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_GC, MDBX_TST_TREE_HEIGHT, 0);
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_GC, MDBX_TST_TREE_LEAF_PAGES, 0);
    mdbx_tst_meta_sign_as_steady(work);
    validate("gc-height0-with-root", MDBX_CORRUPTED);
  }
  {
    /* mod_txnid from the future */
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_MAIN, MDBX_TST_TREE_MOD_TXNID,
                           mdbx_tst_meta_field_get(work, MDBX_TST_META_TXNID_A) + 1);
    mdbx_tst_meta_sign_as_steady(work);
    validate("main-mod-txnid-future", MDBX_CORRUPTED);
  }
  {
    copy_page();
    mdbx_tst_meta_tree_set(work, MDBX_TST_TREE_GC, MDBX_TST_TREE_MOD_TXNID,
                           mdbx_tst_meta_field_get(work, MDBX_TST_META_TXNID_A) + 1);
    mdbx_tst_meta_sign_as_steady(work);
    validate("gc-mod-txnid-future", MDBX_CORRUPTED);
  }
}

/* The mmap_limit bound (MDBX_TOO_LARGE): always reachable through the
 * test-only mdbx_tst_override_mmap_limit() (see the call site). */
TEST(meta_validate_corpus, mmap_limit) {
  env_files_guard guard({kDbPath});
  env_handle_guard henv(&g_env);
  if (MDBX_GTEST_IS_EXECUTION_TIMEOUT()) {
    GTEST_SKIP() << "SKIPPED by RUNTIME_LIMIT";
    return;
  }
  ASSERT_TRUE(setup_corpus());
  /* the default 64-bit limit (MAX_MAPSIZE) cannot be exceeded by a file
   * that fits on disk, so the test-only override lowers it below the
   * database's own size: the case runs on every platform/configuration
   * (previously a >limit sparse file, reachable only under Valgrind) */
  const uint64_t saved = mdbx_tst_override_mmap_limit((uint64_t)file_pages * g_ps / 2);
  copy_page();
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_FIRST_UNALLOCATED, file_pages);
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_UPPER, file_pages);
  mdbx_tst_meta_geo_set(work, MDBX_TST_META_GEO_NOW, file_pages);
  mdbx_tst_meta_sign_as_steady(work);
  expect_rc("allocated-above-mmap-limit", MDBX_TOO_LARGE, mdbx_tst_meta_validate(g_env, work, baseline_pgno, nullptr));
  mdbx_tst_restore_mmap_limit(saved);
}
