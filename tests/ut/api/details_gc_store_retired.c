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

/// V-003 regression test (issue reference: see the PR description).
///
/// Contract: in gc_store_retired (BIGFOOT mode) ANY growth of wr.retired_pages
/// inside a chunk's cursor_put(RESERVE) - what the engine performs when a
/// frozen page is CoW'd during the put (page-ops.c:177 -> pnl_append) - must
/// not break the FIFO arm's `at = left_before - chunk` slicing:
/// the stored GC records must jointly contain exactly the crafted pages plus
/// the appended ones (no junk, nothing dropped), each record must be a valid
/// strictly-descending PNL, and no memory outside the pnl may be written.
///
/// The window: growth g >= 1 during the FIRST chunk (retired_stored == 0) with
/// `chunk_hi >= left_after` (the grown list still fits the chunk, so
/// should_retry stays false). Then `at = N - (N+g) = (size_t)-g`, i.e. a
/// transient negative-index write before the pnl allocation and a corrupted
/// record: `[chunk][foreign/len/alloclen words as pgno][...tail pages]` with
/// the boundary pages dropped. Both chunk_hi branches host such windows:
/// the endgame `(left|3)` branch and the goodchunk branch (goodchunk ≡ 2 mod 4).
///
/// This is a verbatim-extraction test in the details_scan4seq.c style: the
/// gc_store_retired BIGFOOT body below is copied verbatim from src/gc-put.c
/// (@ the fix commit, lines 363-475), the pnl helpers reproduce the pnl.h/pnl.c
/// formulas, and cursor_put is a stub which plays MDBX_RESERVE and injects the
/// growth on the configured call. Keep the extraction in sync when refactoring
/// src/gc-put.c - this test guards the store-slicing contract for refactoring.

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t pgno_t;
typedef uint64_t txnid_t;
typedef struct MDBX_val {
  void *iov_base;
  size_t iov_len;
} MDBX_val;

#define MDBX_SUCCESS 0
#define MDBX_PNL_ASCENDING 0 /* is_lifo() == 0 => the FIFO arm (`left_before - chunk`) */
#define MDBX_ENABLE_BIGFOOT 1
#define MDBX_DEBUG_GCU 0
#define MDBX_DEBUG 0
#define MDBX_LIFORECLAIM 0x4000u
#define MAX_TXNID 0xFFFFFFFEFFFFFFFFull
#define MIN_PAGENO 3
typedef struct MDBX_env {
  unsigned flags;
} MDBX_env;
typedef struct MDBX_cursor {
  int dummy;
} MDBX_cursor;
typedef struct MDBX_txn {
  MDBX_env *env;
  txnid_t txnid;
  struct {
    pgno_t first_unallocated;
  } geo;
  struct {
    pgno_t *__restrict retired_pages;
  } wr;
} MDBX_txn;

/* --- stub state (the single seam) --- */
static MDBX_txn *tst_txn;
static unsigned tst_put_counter;
static unsigned tst_grow_at_put; /* the cursor_put call (0-based) to grow at */
static unsigned tst_grow_amount; /* how many pages to append */
static pgno_t tst_appended_hi;   /* appended pages are tst_appended_hi-i */

static pgno_t *tst_records[64];
static size_t tst_record_slots[64];
static bool tst_abandoned[64]; /* records deleted by gc_clean_stored_retired on retry passes */
static unsigned tst_nrecords;

/* --- pnl.h/pnl.c formulas (see provenance note above) --- */
#define pnl_size(pl) ((pl)[0])
static int tst_pgno_cmp_desc(const void *a, const void *b) {
  const pgno_t x = *(const pgno_t *)a, y = *(const pgno_t *)b;
  return (x < y) - (x > y); /* descending: larger pgno first (FIFO invariant) */
}
static void pnl_sort(pgno_t *pnl, pgno_t limit) {
  /* pnl.h:161 pnl_sort -> pnl_sort_nochk: sorts per the PNL order */
  (void)limit;
  if (pnl[0])
    qsort(pnl + 1, pnl[0], sizeof(pgno_t), tst_pgno_cmp_desc);
}
static void pnl_append_real(pgno_t *__restrict pnl, pgno_t pgno) {
  /* pnl.h tail-append over pre-reserved alloclen */
  const size_t n = pnl[0];
  pnl[n + 1] = pgno;
  pnl[0] = (pgno_t)(n + 1);
}
#define gc_chunk_bytes(length) (((length) + 1) * sizeof(pgno_t))
static inline bool is_lifo(const MDBX_txn *txn) { return (txn->env->flags & MDBX_LIFORECLAIM) != 0; }

/* gcu_t stand-in: exactly the fields the verbatim body touches */
typedef struct gcu_t {
  MDBX_cursor cursor;
  size_t goodchunk;
  size_t retired_stored;
  txnid_t bigfoot;
} gcu_t;

#define tASSERT0(ctx, expr) assert(expr)
#define unlikely(c) __builtin_expect(!!(c), 0)
#define likely(c) __builtin_expect(!!(c), 1)
#define TRACE(...) ((void)0)
#define DEBUG(...) ((void)0)
#define ERROR(...) ((void)0)
#define DEBUG_EXTRA(...) ((void)0)
#define DEBUG_EXTRA_PRINT(...) ((void)0)
#define LOG_ENABLED(lvl) (0)
#define PRIaTXN "lu"
#define PRIaPGNO "u"

/* stubs for the store-loop neighbours (outside the tested contract) */
static int gc_prepare_stockpile4retired(MDBX_txn *txn, gcu_t *ctx) {
  (void)txn;
  (void)ctx;
  return MDBX_SUCCESS;
}
static int gc_clean_stored_retired(MDBX_txn *txn, gcu_t *ctx) {
  /* mirrors the real one: deletes the stored records, rewinds below txnid;
   * the deleted records are marked abandoned so the validators skip them */
  for (unsigned r = 0; r < tst_nrecords; ++r)
    tst_abandoned[r] = true;
  ctx->retired_stored = 0;
  ctx->bigfoot = txn->txnid - 1;
  return MDBX_SUCCESS;
}

/* The single seam: MDBX_RESERVE returns a record buffer; on the configured
 * call the frozen-page CoW appends g pages (tail-append, page-ops.c:177).
 * Named exactly as the library's one: the verbatim body calls cursor_put(). */
static int cursor_put(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data, unsigned flags) {
  (void)mc;
  (void)key;
  (void)flags;
  assert(tst_txn);
  if (tst_put_counter == tst_grow_at_put && tst_grow_amount) {
    for (unsigned i = 0; i < tst_grow_amount; ++i)
      pnl_append_real(tst_txn->wr.retired_pages, (pgno_t)(tst_appended_hi - i));
  }
  tst_put_counter += 1;
  data->iov_base = malloc(data->iov_len);
  assert(data->iov_base != nullptr);
  memset(data->iov_base, 0, data->iov_len);
  assert(tst_nrecords < sizeof(tst_records) / sizeof(tst_records[0]));
  tst_records[tst_nrecords] = (pgno_t *)data->iov_base;
  tst_record_slots[tst_nrecords] = data->iov_len / sizeof(pgno_t);
  tst_nrecords += 1;
  return MDBX_SUCCESS;
}

/*-----------------------------------------------------------------------------*/

/* VERBATIM from src/gc-put.c @ the fix commit (lines 363-475), BIGFOOT branch:
 * keep in sync with the source when refactoring. */
static int gc_store_retired(MDBX_txn *txn, gcu_t *ctx) {
  int err;
  MDBX_val key, data;

#if MDBX_ENABLE_BIGFOOT
  size_t retired_before;
  bool should_retry;
  do {
    if (ctx->bigfoot > txn->txnid) {
      err = gc_clean_stored_retired(txn, ctx);
      if (unlikely(err != MDBX_SUCCESS))
        return err;
      tASSERT0(txn, ctx->bigfoot <= txn->txnid);
    }

    err = gc_prepare_stockpile4retired(txn, ctx);
    if (unlikely(err != MDBX_SUCCESS))
      return err;

    pnl_sort(txn->wr.retired_pages, txn->geo.first_unallocated);
    retired_before = pnl_size(txn->wr.retired_pages);
    should_retry = false;
    ctx->retired_stored = 0;
    ctx->bigfoot = txn->txnid;
    do {
      if (ctx->retired_stored) {
        err = gc_prepare_stockpile4retired(txn, ctx);
        if (unlikely(err != MDBX_SUCCESS))
          return err;
      }
      key.iov_len = sizeof(txnid_t);
      key.iov_base = &ctx->bigfoot;
      const size_t left_before = retired_before - ctx->retired_stored;
      const size_t chunk_hi = ((left_before | 3) > ctx->goodchunk && ctx->bigfoot < (MAX_TXNID - UINT32_MAX))
                                  ? ctx->goodchunk
                                  : (left_before | 3);
      data.iov_len = gc_chunk_bytes(chunk_hi);
      err = cursor_put(&ctx->cursor, &key, &data, 0 /* MDBX_RESERVE */);
      if (unlikely(err != MDBX_SUCCESS))
        return err;

      const size_t retired_after = pnl_size(txn->wr.retired_pages);
      const size_t left_after = retired_after - ctx->retired_stored;
      const size_t chunk = (left_after < chunk_hi) ? left_after : chunk_hi;
      should_retry = retired_before != retired_after;
      if (likely(!should_retry)) {
        const size_t at = (is_lifo(txn) == MDBX_PNL_ASCENDING) ? left_before - chunk : ctx->retired_stored;
        pgno_t *const begin = txn->wr.retired_pages + at;
        const pgno_t save = *begin;
        *begin = (pgno_t)chunk;
        memcpy(data.iov_base, begin, data.iov_len);
        *begin = save;
        (void)at;
      }
      ctx->retired_stored += chunk;
    } while (ctx->retired_stored < pnl_size(txn->wr.retired_pages) && (++ctx->bigfoot, true));
  } while (unlikely(should_retry));
#else
#error "the test targets the BIGFOOT mode"
#endif /* MDBX_ENABLE_BIGFOOT */
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

/* One scenario: N crafted retired pages, growth g pages appended inside the
 * configured cursor_put, FIFO or LIFO arm, goodchunk as given. The pnl is
 * owned by the test with a canary prefix (the negative-index writes land
 * there), alloclen headroom, and descending crafted pages. */
static void tst_scenario(size_t N, unsigned g, unsigned grow_at_put, bool lifo, size_t goodchunk) {
  ++tst_cases;
  static MDBX_env env;
  env.flags = lifo ? MDBX_LIFORECLAIM : 0u;
  MDBX_txn txn;
  memset(&txn, 0, sizeof(txn));
  txn.env = &env;
  txn.txnid = 500;
  const pgno_t fu = 20000; /* any value above all crafted/appended pgno */
  txn.geo.first_unallocated = fu;
  tst_txn = &txn;

  pgno_t *const raw = (pgno_t *)malloc((40 + N + 8) * sizeof(pgno_t));
  assert(raw != nullptr);
  for (unsigned i = 0; i < 32; ++i)
    raw[i] = 0xA5C3A5C3u; /* canaries before the alloclen slot */
  raw[32] = (pgno_t)(N + 8); /* pnl[-1] = alloclen slot, headroom for appends */
  pgno_t *const pnl = raw + 33; /* pnl[0] = length */
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
  ctx.goodchunk = goodchunk;
  ctx.bigfoot = txn.txnid;
  const int rc = gc_store_retired(&txn, &ctx);

  char what[128];
  snprintf(what, sizeof(what), "gc_store_retired rc (N=%zu g=%u grow@%u %s goodchunk=%zu)", N, g, grow_at_put,
           lifo ? "lifo" : "fifo", goodchunk);
  tst_report(what, rc == MDBX_SUCCESS, "rc %d", rc);
  tst_report("retired_stored == N+g", ctx.retired_stored == N + g, "got %zu, want %zu", (size_t)ctx.retired_stored,
             N + g);
  tst_report("pnl length == N+g", pnl_size(pnl) == N + g, "got %u, want %zu", pnl_size(pnl), N + g);

  bool canaries_ok = true;
  for (unsigned i = 0; i < 32; ++i)
    canaries_ok &= raw[i] == 0xA5C3A5C3u;
  tst_report("canaries intact", canaries_ok, nullptr);
  tst_report("alloclen slot intact", raw[32] == (pgno_t)(N + 8), "got %u", raw[32]);

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

  static const size_t small[] = {2, 3, 4, 5, 20, 100, 500};
  for (unsigned s = 0; s < sizeof(small) / sizeof(small[0]); ++s)
    for (unsigned g = 1; g <= 3; ++g)
      for (unsigned lifo = 0; lifo <= 1; ++lifo)
        tst_scenario(small[s], g, 0, lifo != 0, G);

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
