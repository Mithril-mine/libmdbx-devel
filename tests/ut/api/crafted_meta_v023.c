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
 * encodes 65536 pages at a 65536-byte page size carries the byte value 2^32:
 * on a 32-bit host the unfixed code silently wrapped it to zero (growth
 * disabled) and opened such a database, the fixed code refuses it cleanly
 * with MDBX_INCOMPATIBLE.
 *
 * The test runs only where the defect is expressible: on a 32-bit host
 * (a runtime sizeof gate, no preprocessor cut). The android armeabi-v7a and
 * win32 MSVC CI cells are 32-bit and exercise it; on 64-bit hosts it skips.
 * White-box: the crafted meta page is built through the real meta_t/
 * meta_sign helpers compiled from the sources.
 *
 * The crafted meta is written to the winner slot only, with the txnid
 * bumped so the slot wins the troika latch (writing the same meta to all
 * three slots would clash the troika). */

#define debug_log debug_log_sub

#include "../../../src/cogs.c"
#include "../../../src/meta.c"
#include "details-anchors.h"

#include <stdarg.h>

static int debug_verbose;
MDBX_MAYBE_UNUSED __cold void debug_log_sub(int level, const char *function, int line, const char *fmt, ...) {
  if (!debug_verbose)
    return;
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "lib[%d] %s:%d: ", level, function ? function : "?", line);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
}

static const char kDbPath[] = "crafted-v023.mdbx";
static const char kLckPath[] = "crafted-v023.mdbx-lck";

static uint8_t baseline[MDBX_MAX_PAGESIZE];
static uint8_t work[MDBX_MAX_PAGESIZE];

/* the pv encoding of exactly 65536 pages: pagesize 65536 x 65536 pages is
 * the byte value 2^32, which does not fit the 32-bit intptr_t */
static const uint16_t kCraftedGrowPv = 0x8101;

static void cleanup(void) {
  remove(kDbPath);
  remove(kLckPath);
}

int main(void) {
  if (sizeof(size_t) > 4) {
    printf("geometry-from-db-32bit-wrap: SKIPPED on a %u-bit host (the wrap needs a 32-bit size_t)\n",
           (unsigned)(sizeof(void *) * 8));
    return 0;
  }

  cleanup();

  /* a real database at the 65536-byte page size, with an explicit generous
   * geometry (the platform defaults at this page size are tight) */
  MDBX_env *env = nullptr;
  if (mdbx_env_create(&env)) {
    cleanup();
    return 1;
  }
  if (mdbx_env_set_geometry(env, 0, 32 * 1024 * 1024, 512 * 1024 * 1024, 0, 0, 65536)) {
    cleanup();
    return 1;
  }
  if (mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644)) {
    cleanup();
    return 1;
  }
  MDBX_txn *txn = nullptr;
  if (mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn)) {
    cleanup();
    return 1;
  }
  MDBX_dbi dbi;
  if (mdbx_dbi_open(txn, nullptr, MDBX_DB_DEFAULTS, &dbi)) {
    cleanup();
    return 1;
  }
  MDBX_val key = {(void *)"k", 1}, value = {(void *)"v", 1};
  if (mdbx_put(txn, dbi, &key, &value, 0)) {
    cleanup();
    return 1;
  }
  if (mdbx_txn_commit(txn)) {
    cleanup();
    return 1;
  }
  if (mdbx_env_close(env)) {
    cleanup();
    return 1;
  }

  /* snapshot a valid meta page (the corpus pattern) */
  unsigned baseline_pgno = NUM_METAS;
  if (mdbx_env_create(&env)) {
    cleanup();
    return 1;
  }
  if (mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR | MDBX_RDONLY, 0444)) {
    cleanup();
    return 1;
  }
  const size_t ps = env->ps;
  bool found = false;
  for (unsigned n = 0; n < NUM_METAS && !found; ++n) {
    const page_t *page = pgno2page(env, (pgno_t)n);
    memcpy(baseline, page, ps);
    if (meta_validate(env, page_meta((page_t *)(void *)baseline), (const page_t *)baseline, n, nullptr) ==
        MDBX_SUCCESS) {
      found = true;
      baseline_pgno = n;
    }
  }
  mdbx_env_close(env);
  if (!found) {
    printf("geometry-from-db-32bit-wrap: FAIL - no valid meta page in the fresh database\n");
    cleanup();
    return 1;
  }

  /* craft the winner meta slot only (writing the same meta to all three
   * slots would clash the troika): small in-range lower/now/upper/
   * first_unallocated plus the over-range grow_pv; the txnid is bumped so
   * the crafted slot wins the latch; re-signed so the checksum branch
   * passes and the geometry conversion decides the verdict */
  FILE *f = fopen(kDbPath, "r+b");
  if (!f) {
    cleanup();
    return 1;
  }
  memcpy(work, baseline, ps);
  page_t *page = (page_t *)(void *)work;
  page->pgno = (pgno_t)baseline_pgno;
  meta_t *meta = page_meta(page);
  meta->geometry.lower = 3;
  meta->geometry.upper = 1000; /* x pagesize fits the 32-bit intptr_t */
  meta->geometry.now = 4;
  meta->geometry.first_unallocated = 4;
  meta->geometry.grow_pv = kCraftedGrowPv; /* 65536 pages x 65536 = 2^32 bytes */
  meta->geometry.shrink_pv = 0;
  const uint64_t next_txnid = unaligned_peek_u64(4, &meta->txnid_a) + 1;
  unaligned_poke_u64(4, &meta->txnid_a, next_txnid);
  unaligned_poke_u64(4, &meta->txnid_b, next_txnid);
  meta_sign_as_steady(meta);
  fseek(f, (long)baseline_pgno * (long)ps, SEEK_SET);
  if (fwrite(work, 1, ps, f) != ps) {
    cleanup();
    return 1;
  }
  fclose(f);

  /* the reopen must keep the growth usable: the crafted over-range grow
   * step is clamped (geometry-from-db-32bit-wrap) instead of silently wrapping to zero */
  if (mdbx_env_create(&env)) {
    cleanup();
    return 1;
  }
  if (mdbx_env_open(env, kDbPath, MDBX_NOSUBDIR, 0644)) {
    cleanup();
    return 1;
  }
  MDBX_envinfo info;
  memset(&info, 0, sizeof(info));
  if (mdbx_env_info_ex(env, nullptr, &info, sizeof(info))) {
    cleanup();
    return 1;
  }
  mdbx_env_close(env);
  cleanup();

  if (info.mi_geo.grow == 0) {
    printf("geometry-from-db-32bit-wrap: FAIL - the 2^32-byte grow step silently wrapped to zero (growth disabled)\n");
    return 1;
  }
  printf("geometry-from-db-32bit-wrap: PASS - the over-range grow step is preserved, mi_geo_grow = %llu bytes\n",
         (unsigned long long)info.mi_geo.grow);
  return 0;
}
