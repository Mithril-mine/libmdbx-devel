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
/// software or the use or dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/* A corpus of crafted meta pages, one case per reject branch of
 * meta_validate() (src/meta.c): every field mutation below must flip exactly
 * the intended branch and produce the documented return code. The baseline is
 * a real committed meta page snapshotted from a small database, so the
 * positive control (the pristine copy validates as MDBX_SUCCESS) holds on
 * every page size / geometry combination. White-box: the test compiles
 * src/meta.c into the unit (the details_rkl.c pattern).
 *
 * Verification campaign item SC-94 (the reject branches enumerated by
 * wave 2): magic/version, page-pgno, page-flags, pagesize, txnid_a != txnid_b,
 * steady-sign, gc-flags, main-flags, txnid range, geo.lower, geo.upper,
 * first_unallocated, allocated-vs-filesize, the mmap_limit bound (made
 * always-reachable through the test-only mdbx_tst_override_mmap_limit),
 * the #454 height/root/leaf/branch equivalences, false-empty trees and
 * mod_txnid.
 *
 * NOTE: format-dependent (the on-disk meta layout is touched directly through
 * internals.h); must be revisited together with any on-disk format change. */

#define debug_log debug_log_sub

#include "../../../src/cogs.c"
#include "../../../src/meta.c"
#include "details-anchors.h"

MDBX_MAYBE_UNUSED __cold void debug_log_sub(int level, const char *function, int line, const char *fmt, ...) {
  (void)level;
  (void)function;
  (void)line;
  (void)fmt;
}

/*-----------------------------------------------------------------------------*/

static const char kDbPath[] = "meta-validate-corpus.mdbx";
static const char kLckPath[] = "meta-validate-corpus.mdbx-lck";
static unsigned failed, passed;

static MDBX_env *g_env;
static uint8_t baseline[MDBX_MAX_PAGESIZE];
static uint8_t work[MDBX_MAX_PAGESIZE];
static uint32_t g_ps;
static pgno_t file_pages;
static unsigned baseline_pgno;

static void expect_rc(const char *name, int expected, int got) {
  if (got == expected) {
    ++passed;
    return;
  }
  ++failed;
  fflush(nullptr);
  fprintf(stderr, "FAIL %s: expected rc %d, got %d\n", name, expected, got);
  fflush(nullptr);
}

/* A fresh byte copy of the baseline page: the page header and the meta
 * payload overlay the same buffer, exactly like a mapped meta page. */
static page_t *copy_page(void) {
  memcpy(work, baseline, g_ps);
  return (page_t *)(void *)work;
}

/* Validates the current (already mutated and re-signed) work buffer as the
 * meta slot it claims to be (the baseline slot number). */
static void validate(const char *name, int expected) {
  page_t *page = (page_t *)(void *)work;
  expect_rc(name, expected, meta_validate(g_env, page_meta(page), page, baseline_pgno, nullptr));
}

static void setup(void) {
  remove(kDbPath);
  remove(kLckPath);

  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env) != MDBX_SUCCESS)
    exit(EXIT_FAILURE);
  if (mdbx_env_set_geometry(env, 0, 0, 0, 0, 0, 0) != MDBX_SUCCESS)
    exit(EXIT_FAILURE);
  if (mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644) != MDBX_SUCCESS)
    exit(EXIT_FAILURE);
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn) != MDBX_SUCCESS)
    exit(EXIT_FAILURE);
  MDBX_dbi dbi;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi) != MDBX_SUCCESS)
    exit(EXIT_FAILURE);
  MDBX_val key = {(void *)"k", 1}, value = {(void *)"v", 1};
  if (mdbx_put(txn, dbi, &key, &value, 0) != MDBX_SUCCESS)
    exit(EXIT_FAILURE);
  if (mdbx_txn_commit(txn) != MDBX_SUCCESS)
    exit(EXIT_FAILURE);
  if (mdbx_env_close(env) != MDBX_SUCCESS)
    exit(EXIT_FAILURE);

  if (mdbx_env_create(&g_env) != MDBX_SUCCESS)
    exit(EXIT_FAILURE);
  if (mdbx_env_open(g_env, kDbPath, MDBX_NOSUBDIR | MDBX_RDONLY, 0644) != MDBX_SUCCESS)
    exit(EXIT_FAILURE);
  g_ps = g_env->ps;
  if (g_ps > MDBX_MAX_PAGESIZE)
    exit(EXIT_FAILURE);
  file_pages = (pgno_t)(g_env->dxb_mmap.filesize / g_ps);

  /* snapshot a valid meta page as the baseline: at least the troika winner
   * of the committed database must validate successfully */
  bool found = false;
  for (unsigned n = 0; n < NUM_METAS && !found; ++n) {
    const page_t *page = pgno2page(g_env, (pgno_t)n);
    memcpy(work, page, g_ps);
    if (meta_validate(g_env, page_meta((page_t *)(void *)work), (const page_t *)work, n, nullptr) == MDBX_SUCCESS) {
      memcpy(baseline, work, g_ps);
      found = true;
    }
  }
  if (!found) {
    fprintf(stderr, "no valid meta page in the fresh database\n");
    exit(EXIT_FAILURE);
  }
  baseline_pgno = (unsigned)((const page_t *)(const void *)baseline)->pgno;
}

static void teardown(void) {
  if (g_env)
    mdbx_env_close(g_env);
  remove(kDbPath);
  remove(kLckPath);
}

/* Every case below mutates exactly one aspect of the otherwise-valid copy;
 * the sign is refreshed (except for the sign case itself) so the checksum
 * branch passes and the branch under test decides the verdict. */

static void case_magic(void) {
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->magic_and_version[0] ^= 0x5a;
    m->magic_and_version[1] ^= 0xa5;
    const uint64_t v = unaligned_peek_u64(4, &m->magic_and_version);
    expect_rc("bad-magic", (v >> 8) != MDBX_MAGIC ? MDBX_INVALID : MDBX_VERSION_MISMATCH,
              meta_validate(g_env, m, page, baseline_pgno, nullptr));
  }
  {
    /* the magic word intact but an unknown data version: the version is the
     * low byte of the 64-bit magic-and-version word */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    const uint64_t v = unaligned_peek_u64(4, &m->magic_and_version);
    unaligned_poke_u64(4, &m->magic_and_version, (v & ~(uint64_t)0xFF) | 42);
    expect_rc("bad-version", MDBX_VERSION_MISMATCH, meta_validate(g_env, m, page, baseline_pgno, nullptr));
  }
}

static void case_page_header(void) {
  {
    page_t *page = copy_page();
    meta_sign_as_steady(page_meta(page));
    page->pgno += 1;
    validate("wrong-page-pgno", MDBX_INVALID);
  }
  {
    page_t *page = copy_page();
    meta_sign_as_steady(page_meta(page));
    page->flags = P_LEAF;
    validate("wrong-page-flags", MDBX_INVALID);
  }
}

static void case_pagesize(void) {
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->pagesize = 1000 /* deliberately not a power of 2 */;
    meta_sign_as_steady(m);
    validate("pagesize-not-pow2", MDBX_INVALID);
  }
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->pagesize = MDBX_MIN_PAGESIZE / 2;
    meta_sign_as_steady(m);
    validate("pagesize-too-small", MDBX_VERSION_MISMATCH);
  }
}

static void case_txnid_and_sign(void) {
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    unaligned_poke_u64(4, &m->txnid_a, unaligned_peek_u64(4, &m->txnid_b) + 1);
    validate("txnid-a-ne-b", MDBX_RESULT_TRUE);
  }
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    const uint64_t good = meta_sign_calculate(m);
    const uint64_t bad = (good > 2) ? (good ^ 1) : (good + 2);
    m->unsafe_sign = bad;
    validate("steady-sign-mismatch", MDBX_RESULT_TRUE);
  }
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    unaligned_poke_u64(4, &m->txnid_a, 0 /* below MIN_TXNID */);
    unaligned_poke_u64(4, &m->txnid_b, 0);
    meta_sign_as_steady(m);
    validate("txnid-out-of-range", MDBX_RESULT_TRUE);
  }
}

static void case_table_flags(void) {
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.gc.flags = (uint16_t)MDBX_DUPSORT;
    meta_sign_as_steady(m);
    validate("gc-flags", MDBX_INCOMPATIBLE);
  }
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.main.flags = UINT16_MAX;
    meta_sign_as_steady(m);
    validate("main-flags", MDBX_INCOMPATIBLE);
  }
}

static void case_geometry(void) {
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->geometry.lower = 0;
    meta_sign_as_steady(m);
    validate("geo-lower-below-min", MDBX_INVALID);
  }
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->geometry.lower = MAX_PAGENO + 2;
    meta_sign_as_steady(m);
    validate("geo-lower-above-max", MDBX_INVALID);
  }
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->geometry.upper = MAX_PAGENO + 2;
    meta_sign_as_steady(m);
    validate("geo-upper-above-max", MDBX_INVALID);
  }
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->geometry.upper = (m->geometry.lower > MIN_PAGENO) ? m->geometry.lower - 1 : (pgno_t)(MIN_PAGENO - 1);
    meta_sign_as_steady(m);
    validate("geo-upper-below-lower", MDBX_INVALID);
  }
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->geometry.first_unallocated = MAX_PAGENO + 2;
    meta_sign_as_steady(m);
    validate("fua-above-max", MDBX_CORRUPTED);
  }
  {
    /* allocated space beyond the (real) file size */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->geometry.first_unallocated = file_pages + 1;
    m->geometry.upper = file_pages + 1;
    m->geometry.now = file_pages + 1;
    meta_sign_as_steady(m);
    validate("allocated-beyond-filesize", MDBX_CORRUPTED);
  }
  {
    /* lower bound mapping above MAX_MAPSIZE64 is rejected; a bound within
     * the 64-bit envelope is silently clamped instead (the reachable side
     * depends on the platform word width, so the expectation is computed) */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->geometry.lower = MAX_PAGENO + 1;
    m->geometry.upper = MAX_PAGENO + 1;
    m->geometry.now = MAX_PAGENO + 1;
    m->geometry.first_unallocated = file_pages;
    meta_sign_as_steady(m);
    const uint64_t mapsize_min = (uint64_t)(MAX_PAGENO + 1) * m->pagesize;
    validate("mapsize-min-bound", mapsize_min > MAX_MAPSIZE64 ? MDBX_VERSION_MISMATCH : MDBX_SUCCESS);
  }
}

static void case_trees(void) {
  const pgno_t usable_root = page_meta((page_t *)(void *)baseline)->geometry.first_unallocated - 1;
  if (usable_root < NUM_METAS) {
    fprintf(stderr, "SKIP tree cases: no page beyond the meta area\n");
    return;
  }

  {
    /* root beyond first_unallocated */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.main.root = m->geometry.first_unallocated;
    m->trees.main.height = 1;
    m->trees.main.leaf_pages = 1;
    meta_sign_as_steady(m);
    validate("main-root-beyond-fua", MDBX_CORRUPTED);
  }
  {
    /* root inside the meta area */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.main.root = 1;
    m->trees.main.height = 1;
    m->trees.main.leaf_pages = 1;
    meta_sign_as_steady(m);
    validate("main-root-in-metas", MDBX_CORRUPTED);
  }
  {
    /* height beyond the fixed cursor stack (the #454 family) */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.main.root = usable_root;
    m->trees.main.height = CURSOR_STACK_SIZE;
    m->trees.main.leaf_pages = 1;
    meta_sign_as_steady(m);
    validate("main-height-beyond-stack", MDBX_CORRUPTED);
  }
  {
    /* height == 0 with a valid root: the #454 equivalence */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.main.root = usable_root;
    m->trees.main.height = 0;
    m->trees.main.leaf_pages = 0;
    meta_sign_as_steady(m);
    validate("main-height0-with-root", MDBX_CORRUPTED);
  }
  {
    /* height == 1 without leaf pages */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.main.root = usable_root;
    m->trees.main.height = 1;
    m->trees.main.leaf_pages = 0;
    meta_sign_as_steady(m);
    validate("main-height1-without-leaves", MDBX_CORRUPTED);
  }
  {
    /* height > 1 without branch pages */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.main.root = usable_root;
    m->trees.main.height = 2;
    m->trees.main.leaf_pages = 2;
    m->trees.main.branch_pages = 0;
    meta_sign_as_steady(m);
    validate("main-height2-without-branches", MDBX_CORRUPTED);
  }
  {
    /* false-empty: no root, but bookkeeping claims items */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.main.root = P_INVALID;
    m->trees.main.height = 0;
    m->trees.main.leaf_pages = 0;
    m->trees.main.branch_pages = 0;
    m->trees.main.items = 42;
    meta_sign_as_steady(m);
    validate("main-false-empty", MDBX_CORRUPTED);
  }
  {
    /* the same equivalence family for the GC tree */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.gc.root = usable_root;
    m->trees.gc.height = 0;
    m->trees.gc.leaf_pages = 0;
    meta_sign_as_steady(m);
    validate("gc-height0-with-root", MDBX_CORRUPTED);
  }
  {
    /* mod_txnid from the future */
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.main.mod_txnid = unaligned_peek_u64(4, &m->txnid_a) + 1;
    meta_sign_as_steady(m);
    validate("main-mod-txnid-future", MDBX_CORRUPTED);
  }
  {
    page_t *page = copy_page();
    meta_t *m = page_meta(page);
    m->trees.gc.mod_txnid = unaligned_peek_u64(4, &m->txnid_a) + 1;
    meta_sign_as_steady(m);
    validate("gc-mod-txnid-future", MDBX_CORRUPTED);
  }
}

/* The mmap_limit bound (MDBX_TOO_LARGE): always reachable through the
 * test-only mdbx_tst_override_mmap_limit() (see the call site). */
static void case_mmap_limit(void) {
  /* the default 64-bit limit (MAX_MAPSIZE) cannot be exceeded by a file
   * that fits on disk, so the test-only override lowers it below the
   * database's own size: the case runs on every platform/configuration
   * (previously a >limit sparse file, reachable only under Valgrind) */
  const uint64_t saved = mdbx_tst_override_mmap_limit((uint64_t)file_pages * g_ps / 2);
  page_t *page = copy_page();
  meta_t *m = page_meta(page);
  m->geometry.first_unallocated = file_pages;
  m->geometry.upper = file_pages;
  m->geometry.now = file_pages;
  meta_sign_as_steady(m);
  expect_rc("allocated-above-mmap-limit", MDBX_TOO_LARGE, meta_validate(g_env, m, page, baseline_pgno, nullptr));
  mdbx_tst_restore_mmap_limit(saved);
}

/*-----------------------------------------------------------------------------*/

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;
  if (atexit(teardown) != 0)
    return EXIT_FAILURE;
  setup();

  /* --- positive control: the pristine baseline validates ---------------- */
  validate("pristine-baseline", MDBX_SUCCESS);

  case_magic();
  case_page_header();
  case_pagesize();
  case_txnid_and_sign();
  case_table_flags();
  case_geometry();
  case_trees();
  case_mmap_limit();

  printf("meta_validate corpus: %u passed, %u failed\n", passed, failed);
  fflush(nullptr);
  return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
