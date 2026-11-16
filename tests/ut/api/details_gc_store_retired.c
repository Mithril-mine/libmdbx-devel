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

/// V-003 regression test (issue #494, see the PR description).
///
/// The exercised logic is the REAL gc_store_retired() from src/gc-store-retired.h
/// (included below - not a copy, per tests/AGENTS.md §3); src/pnl.c is
/// compiled in for the pnl machinery (the details_pnl.c style). The only
/// seams are the mocked ENVIRONMENT around the store loop (allowed by §3):
/// - cursor_put(): plays MDBX_RESERVE (returns a record buffer) and, on the
///   configured call, tail-appends g pages to wr.retired_pages - the same
///   growth the engine performs when a frozen page is CoW'd inside the put
///   (page-ops.c:177 -> pnl_append);
/// - gc_prepare_stockpile4retired()/gc_clean_stored_retired(): the stockpile
///   prep/cleanup machinery, mocked as always-succeeding (their real bodies
///   drive the cursor machinery which is not the contract under test).
///
/// Contract: for ANY growth timing the FIFO arm's slicing must stay
/// consistent - the stored GC records must jointly contain exactly the
/// crafted pages plus the appended ones (no junk: the length/alloclen counts
/// and foreign heap words must never appear as page numbers, no boundary
/// pages dropped), every record must be a valid strictly-descending PNL, and
/// no memory before the pnl may be written (canaries).
///
/// The window this guards (pre-fix defect, V-003): growth g >= 1 during the
/// FIRST chunk (retired_stored == 0) with `chunk_hi >= left_after` made
/// `at = left_before - chunk` underflow to (size_t)-g - a transient
/// negative-index write before the pnl allocation and a corrupted record.

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The seams are renamed BEFORE the dependency chain is pulled, so the stubs
 * get declared exactly where the library declares the real counterparts:
 *  - cursor_put(): plays MDBX_RESERVE (returns a record buffer) and, on the
 *    configured call, tail-appends g pages to wr.retired_pages - the same
 *    growth the engine performs when a frozen page is CoW'd inside the put
 *    (page-ops.c:177 -> pnl_append);
 *  - debug_log(): silenced (the pnl.c logging sink). */
#define debug_log debug_log_sub
#define cursor_put tst_cursor_put_stub
/* renaming the two stockpile hooks too: their REAL definitions live in
 * gc-put.c, and this TU must not collide with them (the archive member gets
 * pulled transitively by the logging symbols) */
#define gc_prepare_stockpile4retired tst_prep_stockpile4stub
#define gc_clean_stored_retired tst_clean_stored_stub

/* src/pnl.c for the real pnl machinery (pnl_sort -> pnl_sort_nochk) */
#include "../../../src/pnl.c"

/* the REAL store loop (src/gc-store-retired.h) */
#include "../../../src/gc-store-retired.h"

#undef cursor_put
#undef gc_prepare_stockpile4retired
#undef gc_clean_stored_retired
#undef debug_log

MDBX_MAYBE_UNUSED __cold void debug_log_sub(int level, const char *function, int line, const char *fmt, ...) {
  (void)level;
  (void)function;
  (void)line;
  (void)fmt;
}

/*-----------------------------------------------------------------------------*/

/* --- stub state (the mocked environment) --- */
static MDBX_txn *tst_txn;
static unsigned tst_put_counter;
static unsigned tst_grow_at_put; /* the cursor_put call (0-based) to grow at */
static unsigned tst_grow_amount; /* how many pages to append */
static pgno_t tst_appended_hi;   /* appended pages are tst_appended_hi-i */

static pgno_t *tst_records[64];
static size_t tst_record_slots[64];
static bool tst_abandoned[64]; /* records deleted by gc_clean_stored_retired on retry passes */
static unsigned tst_nrecords;

int tst_cursor_put_stub(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data, unsigned flags) {
  (void)mc;
  (void)key;
  (void)flags;
  assert(tst_txn && mc->txn == tst_txn);
  if (tst_put_counter == tst_grow_at_put && tst_grow_amount) {
    /* the engine's frozen-page CoW: pnl_append tail-appends (page-ops.c:177);
     * the crafted pnl has alloclen headroom, so no realloc happens */
    for (unsigned i = 0; i < tst_grow_amount; ++i) {
      const int rc = pnl_append(&tst_txn->wr.retired_pages, (pgno_t)(tst_appended_hi - i));
      assert(rc == MDBX_SUCCESS);
      (void)rc;
    }
  }
  tst_put_counter += 1;
  data->iov_base = malloc(data->iov_len);
  assert(data->iov_base != nullptr);
  memset(data->iov_base, 0, data->iov_len); /* RESERVE leaves the buffer uninitialized */
  assert(tst_nrecords < sizeof(tst_records) / sizeof(tst_records[0]));
  tst_records[tst_nrecords] = (pgno_t *)data->iov_base;
  tst_record_slots[tst_nrecords] = data->iov_len / sizeof(pgno_t);
  tst_nrecords += 1;
  return MDBX_SUCCESS;
}

MDBX_MAYBE_UNUSED int tst_prep_stockpile4stub(MDBX_txn *txn, gcu_t *ctx) {
  (void)txn;
  (void)ctx;
  return MDBX_SUCCESS;
}

MDBX_MAYBE_UNUSED int tst_clean_stored_stub(MDBX_txn *txn, gcu_t *ctx) {
  /* mirrors the real one: deletes the stored records, rewinds below txnid;
   * the deleted records are marked abandoned so the validators skip them */
  for (unsigned r = 0; r < tst_nrecords; ++r)
    tst_abandoned[r] = true;
  ctx->retired_stored = 0;
  ctx->bigfoot = txn->txnid - 1;
  return MDBX_SUCCESS;
}

/*-----------------------------------------------------------------------------*/

static size_t tst_failed, tst_ok, tst_cases;

static void tst_report(const char *what, bool ok, const char *fmt, ...) {
  if (ok) {
    ++tst_ok;
    return;
  }
  ++tst_failed;
  fprintf(stderr, "  FAIL: %s", what);
  if (fmt && fmt[0]) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, ": ");
    vfprintf(stderr, fmt, args);
    va_end(args);
  }
  fprintf(stderr, "\n");
}

static int tst_pgno_cmp_desc(const void *a, const void *b) {
  const pgno_t x = *(const pgno_t *)a, y = *(const pgno_t *)b;
  return (x < y) - (x > y); /* descending: larger pgno first (FIFO invariant) */
}

/* One scenario: N crafted retired pages, growth g pages appended inside the
 * configured cursor_put, FIFO or LIFO arm, goodchunk as given. The pnl is
 * owned by the test with a canary prefix (negative-index writes land there),
 * alloclen headroom, and descending crafted pages. */
static void tst_scenario(size_t N, unsigned g, unsigned grow_at_put, bool lifo, size_t goodchunk) {
  ++tst_cases;
  static MDBX_env env;
  memset(&env, 0, sizeof(env));
  env.flags = lifo ? MDBX_LIFORECLAIM : 0u;
  MDBX_txn txn;
  memset(&txn, 0, sizeof(txn));
  txn.env = &env;
  txn.txnid = 500;
  const pgno_t fu = 20000; /* any value above all crafted/appended pgno */
  txn.geo.first_unallocated = fu;
  tst_txn = &txn;

  /* The pnl is allocated per the real engine's conventions: the radix-sort
   * scratch of the real pnl_sort lives in the tail beyond the data, so the
   * allocation must be ~2x the size (see pnl_size2bytes' doubling) plus the
   * headroom for the appended pages and the canary prefix. */
  pgno_t *const raw = (pgno_t *)malloc((32 + 2 + 2 * N + 64) * sizeof(pgno_t));
  assert(raw != nullptr);
  for (unsigned i = 0; i < 32; ++i)
    raw[i] = 0xA5C3A5C3u;         /* canaries before the alloclen slot */
  raw[32] = (pgno_t)(2 * N + 64); /* pnl[-1] = alloclen slot (radix scratch + growth headroom) */
  pgno_t *const pnl = raw + 33;   /* pnl[0] = length */
  pnl[0] = (pgno_t)N;
  for (size_t i = 0; i < N; ++i)
    pnl[1 + i] = (pgno_t)(fu - 100 - 2 * i); /* descending, distinct, far from junk values */
  txn.wr.retired_pages = pnl;

  tst_appended_hi = (pgno_t)(fu - 1);
  tst_put_counter = 0;
  tst_grow_at_put = grow_at_put;
  tst_grow_amount = g;
  tst_nrecords = 0;
  memset(tst_abandoned, 0, sizeof(tst_abandoned));

  gcu_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.cursor.txn = &txn;               /* the real flow sets it via cursor_init() */
  ctx.goodchunk = (unsigned)goodchunk; /* gcu_t stores it as unsigned */
  ctx.bigfoot = txn.txnid;
  const int rc = gc_store_retired(&txn, &ctx);

  char what[128];
  snprintf(what, sizeof(what), "gc_store_retired rc (N=%zu g=%u grow@%u %s goodchunk=%zu)", N, g, grow_at_put,
           lifo ? "lifo" : "fifo", goodchunk);
  tst_report(what, rc == MDBX_SUCCESS, "rc %d", rc);
  tst_report("retired_stored == N+g", ctx.retired_stored == N + g, "got %zu, want %zu", (size_t)ctx.retired_stored,
             N + g);
  tst_report("pnl length == N+g", pnl_size(pnl) == N + g, "got %u, want %zu", (unsigned)pnl_size(pnl), N + g);

  bool canaries_ok = true;
  for (unsigned i = 0; i < 32; ++i)
    canaries_ok &= raw[i] == 0xA5C3A5C3u;
  tst_report("canaries intact", canaries_ok, nullptr);
  tst_report("alloclen slot intact", raw[32] == (pgno_t)(2 * N + 64), "got %u, want %u", raw[32], (pgno_t)(2 * N + 64));

  /* The stored records must jointly hold exactly the crafted pages plus the
   * appended ones - no junk, nothing dropped. */
  const size_t expect_n = N + g;
  pgno_t *expect = (pgno_t *)malloc((expect_n + 1) * sizeof(pgno_t));
  size_t expect_count = 0;
  for (size_t i = 0; i < N; ++i)
    expect[expect_count++] = (pgno_t)(fu - 100 - 2 * i);
  for (unsigned i = 0; i < g; ++i)
    expect[expect_count++] = (pgno_t)(tst_appended_hi - i);
  qsort(expect, expect_count, sizeof(pgno_t), tst_pgno_cmp_desc);

  pgno_t *got = (pgno_t *)malloc((expect_n + 1) * sizeof(pgno_t));
  size_t got_count = 0;
  bool records_valid = true, counts_ok = true, order_ok = true;
  size_t stored_pages = 0;
  for (unsigned r = 0; r < tst_nrecords; ++r) {
    if (tst_abandoned[r])
      continue; /* deleted by gc_clean_stored_retired on a retry pass */
    const pgno_t *rec = tst_records[r];
    const size_t count = rec[0];
    if (count == 0)
      continue; /* a zeroed buffer = the put stored nothing (retry skipped it) */
    counts_ok &= count <= tst_record_slots[r] - 1;
    if (count > tst_record_slots[r] - 1)
      continue;
    stored_pages += count;
    for (size_t i = 1; i <= count; ++i) {
      const pgno_t v = rec[i];
      if (v < MIN_PAGENO || v >= fu)
        records_valid = false;
      if (got_count < expect_n)
        got[got_count++] = v;
    }
    for (size_t i = 1; i < count; ++i)
      order_ok &= rec[i] > rec[i + 1]; /* strictly descending PNL */
  }
  tst_report("every record: sane count", counts_ok, nullptr);
  tst_report("every record: valid pgno range", records_valid, nullptr);
  tst_report("every record: strictly descending", order_ok, nullptr);
  tst_report("total stored pages == N+g", stored_pages == expect_n, "got %zu, want %zu", stored_pages, expect_n);

  bool multiset_ok = (got_count == expect_count);
  if (multiset_ok) {
    qsort(got, got_count, sizeof(pgno_t), tst_pgno_cmp_desc);
    for (size_t i = 0; i < expect_count; ++i)
      multiset_ok &= got[i] == expect[i];
  }
  tst_report("records == crafted+appended, no junk, nothing dropped", multiset_ok, nullptr);
  if (!multiset_ok || !order_ok || !records_valid) {
    fprintf(stderr, "    case N=%zu g=%u grow@%u %s goodchunk=%zu\n", N, g, grow_at_put, lifo ? "lifo" : "fifo",
            goodchunk);
    for (unsigned r = 0; r < tst_nrecords; ++r) {
      if (tst_abandoned[r])
        continue;
      fprintf(stderr, "    record#%u count=%zu:", r, (size_t)tst_records[r][0]);
      const size_t count = tst_records[r][0];
      for (size_t i = 1; i <= count && i < 8; ++i)
        fprintf(stderr, " %u", tst_records[r][i]);
      fprintf(stderr, "%s\n", count > 7 ? " ..." : "");
    }
  }

  free(expect);
  free(got);
  for (unsigned r = 0; r < tst_nrecords; ++r)
    free(tst_records[r]);
  tst_nrecords = 0;
  free(raw);
  tst_txn = nullptr;
}

/*-----------------------------------------------------------------------------*/

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  /* goodchunk values below mirror the real ones for 4K pages
   * (maxgc_large1page - maxgc_large1page/16); the arithmetic under test is
   * goodchunk-relative, so any consistent pair exercises the same windows. */
  const size_t G = 955;  /* ≡ 3 (mod 4): the endgame (left|3) branch hosts windows */
  const size_t GG = 958; /* ≡ 2 (mod 4): the goodchunk branch hosts windows */

  /* NOT named `small`: the Windows SDK macro rpcndr.h defines small as char */
  static const size_t tst_small[] = {2, 3, 4, 5, 20, 100, 500};
  for (unsigned s = 0; s < sizeof(tst_small) / sizeof(tst_small[0]); ++s)
    for (unsigned g = 1; g <= 3; ++g)
      for (unsigned lifo = 0; lifo <= 1; ++lifo)
        tst_scenario(tst_small[s], g, 0, lifo != 0, G);

  /* the endgame (left|3) branch: every residue except 3 opens for some g */
  for (size_t d = 0; d <= 4; ++d)
    for (unsigned g = 1; g <= 3; ++g)
      for (unsigned lifo = 0; lifo <= 1; ++lifo)
        tst_scenario(G - d, g, 0, lifo != 0, G);

  /* multi-chunk: growth on the first put is retried when it exceeds the
   * chunk (safe); chunk-boundary behaviour with growth */
  for (unsigned g = 1; g <= 3; ++g)
    for (unsigned lifo = 0; lifo <= 1; ++lifo)
      tst_scenario(2 * G + 7, g, 0, lifo != 0, G);

  /* the goodchunk branch window (goodchunk ≡ 2 mod 4, N ≈ goodchunk-2) */
  for (unsigned g = 1; g <= 3; ++g)
    for (unsigned lifo = 0; lifo <= 1; ++lifo)
      tst_scenario(GG - 2, g, 0, lifo != 0, GG);

  /* sanity: no growth at all must behave identically in both arms */
  for (unsigned lifo = 0; lifo <= 1; ++lifo)
    tst_scenario(500, 0, 0, lifo != 0, G);
  /* growth on a later put (retired_stored > 0 -> always retried) */
  for (unsigned g = 1; g <= 3; ++g)
    for (unsigned lifo = 0; lifo <= 1; ++lifo)
      tst_scenario(2 * G + 7, g, 1, lifo != 0, G);

  printf("cases=%zu ok=%zu failed=%zu\n", tst_cases, tst_ok, tst_failed);
  if (tst_failed) {
    fprintf(stderr, "FAILED %zu checks\n", tst_failed);
    return 1;
  }
  return 0;
}
