/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/* The implementation of the testing API; see tests/api.h for the contract
 * and tests/AGENTS.md for the rules. This file is compiled into the
 * library itself: src/alloy.c includes it under #if MDBX_ENABLE_TESTS (so
 * the wrappers reach the internal interfaces inside the same translation
 * unit), and the non-alloy CMake build appends it to the source list.
 *
 * Naming rules: every public function here is prefixed with `mdbx_tst_`,
 * every private (implementation-internal) function/type with `tst_`. As
 * this API grows, the monolithic tests/api.c will be split into per-area
 * parts in the tests/api/ directory. */

#include "../src/internals.h"

#include "../src/gc-scan4seq.h"

/* The public mirror of the list-element type must stay in sync with the
 * internal one; a drift here is a build break, not a silent test skew. */
STATIC_ASSERT_MSG(sizeof(uint32_t) == sizeof(pgno_t), "scan4seq element type");
STATIC_ASSERT_MSG(PAGEHDRSZ == MDBX_TST_PAGEHDRSZ, "page header size");

/* The public mirror constants must stay in sync with the internal ones;
 * a drift here is a build break, not a silent test skew. */
STATIC_ASSERT_MSG(MDBX_TST_WALK_DONT_CHECK_KEYS_ORDERING == (unsigned)dont_check_keys_ordering, "walk options");
STATIC_ASSERT_MSG(MDBX_TST_WALK_DONT_WALK_GC == (unsigned)dont_walk_GC, "walk options");
STATIC_ASSERT_MSG(MDBX_TST_WALK_DONT_WALK_MAIN == (unsigned)dont_walk_MAIN, "walk options");
STATIC_ASSERT_MSG(MDBX_TST_WALK_DONT_CHECK_REVISIT == (unsigned)dont_check_revisit, "walk options");
/* the scan4seq wrappers expose the list element as a plain uint32_t */

STATIC_ASSERT_MSG(MDBX_TST_PAGE_BROKEN == (int)page_broken, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_LARGE == (int)page_large, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_BRANCH == (int)page_branch, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_LEAF == (int)page_leaf, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_DUPFIX_LEAF == (int)page_dupfix_leaf, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_SUB_LEAF == (int)page_sub_leaf, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_SUB_DUPFIX_LEAF == (int)page_sub_dupfix_leaf, "page type");

STATIC_ASSERT_MSG(MDBX_TST_Z_INNER == (unsigned)z_inner, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_GCU_PREPARATION == (unsigned)z_gcu_preparation, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_FRESH == (unsigned)z_fresh, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_AFTER_DELETE == (unsigned)z_after_delete, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_INNER_TAIL_DELETED == (unsigned)z_inner_tail_deleted, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_EOF_SOFT == (unsigned)z_eof_soft, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_EOF_HARD == (unsigned)z_eof_hard, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_HOLLOW == (unsigned)(uint8_t)z_hollow, "cursor zone");

STATIC_ASSERT_MSG(MDBX_TST_NUM_METAS == NUM_METAS, "meta pages count");

/*----------------------------------------------------------------------------*/

struct tst_walk_ctx {
  MDBX_tst_walk_func *visitor;
  void *ctx;
  MDBX_txn *txn;
};

__cold static int tst_walk_adapter(const size_t pgno, const unsigned number, void *const ctx_, const unsigned deep,
                                   const walk_tbl_t *const table, const size_t page_size, const page_type_t page_type,
                                   const txnid_t page_txnid, const MDBX_error_t err, const size_t nentries,
                                   const size_t payload_bytes, const size_t header_bytes, const size_t unused_bytes,
                                   const size_t parent_pgno) {
  const struct tst_walk_ctx *const wrap = (const struct tst_walk_ctx *)ctx_;
  const char *name;
  size_t name_len;
  if (table->name.iov_base == MDBX_CHK_GC) {
    name = "GC";
    name_len = 2;
  } else if (table->name.iov_base == MDBX_CHK_MAIN) {
    name = "MAIN";
    name_len = 4;
  } else {
    name = (const char *)table->name.iov_base;
    name_len = table->name.iov_len;
  }
  return wrap->visitor(wrap->ctx, wrap->txn, pgno, number, deep, name, name_len, table->nested != nullptr, page_size,
                       (int)page_type, page_txnid, (int)err, nentries, payload_bytes, header_bytes, unused_bytes,
                       parent_pgno);
}

__cold int mdbx_tst_walk_pages(MDBX_txn *txn, MDBX_tst_walk_func *visitor, void *ctx, unsigned options) {
  if (unlikely(!visitor))
    return MDBX_EINVAL;
  const struct tst_walk_ctx wrap = {visitor, ctx, txn};
  return walk_pages(txn, tst_walk_adapter, (void *)&wrap, (walk_options_t)options);
}

/*----------------------------------------------------------------------------*/

__cold static size_t tst_clamp_limit(uint64_t value) { return (value > (uint64_t)SIZE_MAX) ? SIZE_MAX : (size_t)value; }

__cold uint64_t mdbx_tst_override_mmap_limit(uint64_t limit) {
  const uint64_t previous = globals.mmap_limit;
  globals.mmap_limit = tst_clamp_limit(limit);
  return previous;
}

__cold void mdbx_tst_restore_mmap_limit(uint64_t previous) { globals.mmap_limit = tst_clamp_limit(previous); }

/*----------------------------------------------------------------------------*/

__cold bool mdbx_tst_troika_verify_fsm(void) { return troika_verify_fsm(); }

__cold bool mdbx_tst_pv2pages_verify(void) { return pv2pages_verify(); }

__cold uint64_t mdbx_tst_geo_first_unallocated(const MDBX_txn *txn) {
  if (unlikely(!txn))
    return 0;
  return (uint64_t)txn->geo.first_unallocated;
}

__cold int mdbx_tst_cursor_state(const MDBX_cursor *mc, unsigned *zone_flags, int *top, unsigned *ki_top) {
  if (unlikely(!mc))
    return MDBX_EINVAL;
  const MDBX_cursor *const inner = mc;
  if (zone_flags)
    *zone_flags = (unsigned)(uint8_t)inner->flags;
  if (top)
    *top = inner->top;
  if (ki_top)
    *ki_top = (inner->top >= 0) ? (unsigned)inner->ki[inner->top] : 0;
  return MDBX_SUCCESS;
}

unsigned mdbx_tst_scan4seq_variants_mask(void) {
  unsigned mask = 1u << MDBX_TST_SCAN4SEQ_FALLBACK;
#if defined(MDBX_ATTRIBUTE_TARGET_SSE2) && MDBX_HAVE_BUILTIN_CPU_SUPPORTS
  if (__builtin_cpu_supports("sse2"))
    mask |= 1u << MDBX_TST_SCAN4SEQ_SSE2;
#endif /* MDBX_ATTRIBUTE_TARGET_SSE2 */
#if defined(MDBX_ATTRIBUTE_TARGET_AVX2) && MDBX_HAVE_BUILTIN_CPU_SUPPORTS
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("sse2"))
    mask |= 1u << MDBX_TST_SCAN4SEQ_AVX2;
#endif /* MDBX_ATTRIBUTE_TARGET_AVX2 */
#if defined(MDBX_ATTRIBUTE_TARGET_AVX512BW) && MDBX_HAVE_BUILTIN_CPU_SUPPORTS
  if (__builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx2"))
    mask |= 1u << MDBX_TST_SCAN4SEQ_AVX512BW;
#endif /* MDBX_ATTRIBUTE_TARGET_AVX512BW */
  return mask;
}

__cold static uint32_t *tst_scan4seq_unavailable(uint32_t *range, size_t len, size_t seq) {
  (void)range;
  (void)len;
  (void)seq;
  return nullptr;
}

__cold uint32_t *mdbx_tst_scan4seq_variant(unsigned variant, uint32_t *range, size_t len, size_t seq) {
  pgno_t *const result = (pgno_t *)range;
  switch (variant) {
  default:
    return nullptr;
  case MDBX_TST_SCAN4SEQ_FALLBACK:
    return (uint32_t *)scan4seq_fallback(result, len, seq);
  case MDBX_TST_SCAN4SEQ_IMPL:
    return (uint32_t *)scan4seq_impl(result, len, seq);
#if defined(MDBX_ATTRIBUTE_TARGET_SSE2) && MDBX_HAVE_BUILTIN_CPU_SUPPORTS
  case MDBX_TST_SCAN4SEQ_SSE2:
    return (uint32_t *)scan4seq_sse2(result, len, seq);
#endif /* MDBX_ATTRIBUTE_TARGET_SSE2 */
#if defined(MDBX_ATTRIBUTE_TARGET_AVX2) && MDBX_HAVE_BUILTIN_CPU_SUPPORTS
  case MDBX_TST_SCAN4SEQ_AVX2:
    return (uint32_t *)scan4seq_avx2(result, len, seq);
#endif /* MDBX_ATTRIBUTE_TARGET_AVX2 */
#if defined(MDBX_ATTRIBUTE_TARGET_AVX512BW) && MDBX_HAVE_BUILTIN_CPU_SUPPORTS
  case MDBX_TST_SCAN4SEQ_AVX512BW:
    return (uint32_t *)scan4seq_avx512bw(result, len, seq);
#endif /* MDBX_ATTRIBUTE_TARGET_AVX512BW */
  }
  return tst_scan4seq_unavailable(range, len, seq);
}

__cold const uint32_t *mdbx_tst_scan4seq_checker(const uint32_t *pnl, size_t seq) {
  /* the internal checker's pnl_t parameter is a non-const list pointer
   * (it never writes through it); the const cast is confined here */
  pgno_t *const list = (pgno_t *)((uintptr_t)pnl);
  return (const uint32_t *)scan4range_checker(list, seq);
}

/* ------------------------------------------------------------------------ */
/* The gc_store_retired hook-replacement harness (the gc-store-retired-growth-corruption regression
 * scenario, issue #495): the REAL src/gc-store-retired.h store loop is
 * compiled into this unit with its callees replaced by the stubs below - a
 * stack MDBX_env/MDBX_txn with a crafted retired list (canary prefix +
 * alloclen headroom per the engine's pnl conventions) and a cursor_put stub
 * that injects the in-window retired-pages growth. The multi-instance
 * pattern: gc-put.c includes the same header for the REAL instance, so the
 * instrumented entry and callees are macro-renamed here (no #pragma once in
 * that header - the two instances coexist in one translation unit). */

/* the stubs, declared before the header include: the instrumented instance
 * inside src/gc-store-retired.h calls them through the macro renames */
static int tst_cursor_put_stub(MDBX_cursor *mc, const MDBX_val *key, MDBX_val *data, unsigned flags);
static int tst_prep_stockpile4stub(MDBX_txn *txn, gcu_t *ctx);
static int tst_clean_stored_stub(MDBX_txn *txn, gcu_t *ctx);

#define cursor_put tst_cursor_put_stub
#define gc_prepare_stockpile4retired tst_prep_stockpile4stub
#define gc_clean_stored_retired tst_clean_stored_stub
#define gc_store_retired tst_gc_store_retired_instrumented
#define gc_is_lifo tst_gc_is_lifo_instrumented
#define gc_dbg_prefix tst_gc_dbg_prefix_instrumented
#include "../src/gc-store-retired.h"
#undef cursor_put
#undef gc_prepare_stockpile4retired
#undef gc_clean_stored_retired
#undef gc_store_retired
#undef gc_is_lifo
#undef gc_dbg_prefix

/* --- stub state (the mocked environment) --- */
static MDBX_txn *tst_txn;
static unsigned tst_put_counter;
static unsigned tst_grow_at_put; /* the cursor_put call (0-based) to grow at */
static unsigned tst_grow_amount; /* how many pages to append */
static uint32_t tst_appended_hi; /* appended pages are tst_appended_hi-i */

static uint32_t *tst_records[64];
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
      const int rc = pnl_append(&tst_txn->wr.retired_pages, (uint32_t)(tst_appended_hi - i));
      assert(rc == MDBX_SUCCESS);
      (void)rc;
    }
  }
  tst_put_counter += 1;
  data->iov_base = malloc(data->iov_len);
  assert(data->iov_base != nullptr);
  memset(data->iov_base, 0, data->iov_len); /* RESERVE leaves the buffer uninitialized */
  assert(tst_nrecords < sizeof(tst_records) / sizeof(tst_records[0]));
  tst_records[tst_nrecords] = (uint32_t *)data->iov_base;
  tst_record_slots[tst_nrecords] = data->iov_len / sizeof(uint32_t);
  tst_nrecords += 1;
  return MDBX_SUCCESS;
}

MDBX_MAYBE_UNUSED static int tst_prep_stockpile4stub(MDBX_txn *txn, gcu_t *ctx) {
  (void)txn;
  (void)ctx;
  return MDBX_SUCCESS;
}

MDBX_MAYBE_UNUSED static int tst_clean_stored_stub(MDBX_txn *txn, gcu_t *ctx) {
  /* mirrors the real one: deletes the stored records, rewinds below txnid;
   * the deleted records are marked abandoned so the validators skip them */
  (void)txn;
  for (unsigned r = 0; r < tst_nrecords; ++r)
    tst_abandoned[r] = true;
  ctx->retired_stored = 0;
#if MDBX_ENABLE_BIGFOOT /* the gcu_t member is option-gated */
  ctx->bigfoot = txn->txnid - 1;
#endif /* MDBX_ENABLE_BIGFOOT */
  return MDBX_SUCCESS;
}

/*---------------------------------------------------------------------------*/

static size_t tst_failed_checks, tst_ok_checks, tst_cases;

/* no direct stdio here: the harness is compiled INTO the library, and some
 * configurations (MDBX_WITHOUT_MSVC_CRT) have no CRT stdio to link against */
static void tst_report(const char *what, bool ok) {
  if (ok) {
    ++tst_ok_checks;
    return;
  }
  ++tst_failed_checks;
  ERROR("tst: FAILED check: %s", what);
}

/* One scenario: N crafted retired pages, growth g pages appended inside the
 * configured cursor_put, FIFO or LIFO arm, goodchunk as given. The pnl is
 * owned by the harness with a canary prefix (negative-index writes land
 * there), alloclen headroom, and descending crafted pages. */
static void tst_scenario(size_t N, unsigned g, unsigned grow_at_put, bool lifo, size_t goodchunk) {
  ++tst_cases;
  static MDBX_env env;
  memset(&env, 0, sizeof(env));
  env.flags = lifo ? MDBX_LIFORECLAIM : 0u;
  MDBX_txn txn;
  memset(&txn, 0, sizeof(txn));
  txn.env = &env;
  txn.txnid = 500;
  const uint32_t fu = 20000; /* any value above all crafted/appended pgno */
  txn.geo.first_unallocated = fu;
  tst_txn = &txn;

  /* The pnl is allocated per the real engine's conventions: the radix-sort
   * scratch of the real pnl_sort lives in the tail beyond the data, so the
   * allocation must be ~2x the size (see pnl_size2bytes' doubling) plus the
   * headroom for the appended pages and the canary prefix. */
  uint32_t *const raw = (uint32_t *)malloc((32 + 2 + 2 * N + 64) * sizeof(uint32_t));
  assert(raw != nullptr);
  for (unsigned i = 0; i < 32; ++i)
    raw[i] = 0xA5C3A5C3u;           /* canaries before the alloclen slot */
  raw[32] = (uint32_t)(2 * N + 64); /* pnl[-1] = alloclen slot (radix scratch + growth headroom) */
  uint32_t *const pnl = raw + 33;   /* pnl[0] = length */
  pnl[0] = (uint32_t)N;
  for (size_t i = 0; i < N; ++i)
    pnl[1 + i] = (uint32_t)(fu - 100 - 2 * i); /* descending, distinct, far from junk values */
  txn.wr.retired_pages = pnl;

  tst_appended_hi = (uint32_t)(fu - 1);
  tst_put_counter = 0;
  tst_grow_at_put = grow_at_put;
  tst_grow_amount = g;
  tst_nrecords = 0;
  memset(tst_abandoned, 0, sizeof(tst_abandoned));

  gcu_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.cursor.txn = &txn;               /* the real flow sets it via cursor_init() */
  ctx.goodchunk = (unsigned)goodchunk; /* gcu_t stores it as unsigned */
#if MDBX_ENABLE_BIGFOOT                /* the gcu_t member is option-gated */
#if MDBX_ENABLE_BIGFOOT                /* the gcu_t member is option-gated */
  ctx.bigfoot = txn.txnid;
#endif /* MDBX_ENABLE_BIGFOOT */
#endif /* MDBX_ENABLE_BIGFOOT */
  const int rc = tst_gc_store_retired_instrumented(&txn, &ctx);

  char what[128];
  snprintf(what, sizeof(what), "gc_store_retired rc (N=%zu g=%u grow@%u %s goodchunk=%zu)", N, g, grow_at_put,
           lifo ? "lifo" : "fifo", goodchunk);
  tst_report(what, rc == MDBX_SUCCESS);
  snprintf(what, sizeof(what), "retired_stored == N+g (N=%zu g=%u)", N, g);
  tst_report(what, ctx.retired_stored == N + g);
  snprintf(what, sizeof(what), "pnl length == N+g (N=%zu)", N);
  tst_report(what, pnl_size(pnl) == N + g);

  bool canaries_ok = true;
  for (unsigned i = 0; i < 32; ++i)
    canaries_ok &= raw[i] == 0xA5C3A5C3u;
  tst_report("canaries intact", canaries_ok);
  snprintf(what, sizeof(what), "alloclen slot intact (N=%zu)", N);
  tst_report(what, raw[32] == (uint32_t)(2 * N + 64));

  /* The stored records must jointly hold exactly the crafted pages plus the
   * appended ones - no junk, nothing dropped. */
  const size_t expect_n = N + g;
  uint32_t *expect = (uint32_t *)malloc((expect_n + 1) * sizeof(uint32_t));
  size_t expect_count = 0;
  for (size_t i = 0; i < N; ++i)
    expect[expect_count++] = (uint32_t)(fu - 100 - 2 * i);
  for (unsigned i = 0; i < g; ++i)
    expect[expect_count++] = (uint32_t)(tst_appended_hi - i);

  uint32_t *got = (uint32_t *)malloc((expect_n + 1) * sizeof(uint32_t));
  size_t got_count = 0;
  bool records_valid = true, counts_ok = true, order_ok = true;
  size_t stored_pages = 0;
  for (unsigned r = 0; r < tst_nrecords; ++r) {
    if (tst_abandoned[r])
      continue; /* deleted by gc_clean_stored_retired on a retry pass */
    const uint32_t *rec = tst_records[r];
    const size_t count = rec[0];
    if (count == 0)
      continue; /* a zeroed buffer = the put stored nothing (retry skipped it) */
    counts_ok &= count <= tst_record_slots[r] - 1;
    if (count > tst_record_slots[r] - 1)
      continue;
    stored_pages += count;
    for (size_t i = 1; i <= count; ++i) {
      const uint32_t v = rec[i];
      if (v < 3 || v >= fu)
        records_valid = false;
      if (got_count < expect_n)
        got[got_count++] = v;
    }
    for (size_t i = 1; i < count; ++i)
      order_ok &= rec[i] > rec[i + 1]; /* strictly descending PNL */
  }
  tst_report("every record: sane count", counts_ok);
  tst_report("every record: valid pgno range", records_valid);
  tst_report("every record: strictly descending", order_ok);
  snprintf(what, sizeof(what), "total stored pages == N+g (N=%zu g=%u)", N, g);
  tst_report(what, stored_pages == expect_n);

  bool multiset_ok = (got_count == expect_count);
  if (multiset_ok) {
    /* descending compare without qsort: both sides are crafted descending,
     * but the stored concatenation is per-chunk - sort a small copy */
    for (size_t i = 0; i < expect_count; ++i)
      for (size_t k = i + 1; k < expect_count; ++k)
        if (got[k] > got[i]) {
          const uint32_t tmp = got[i];
          got[i] = got[k];
          got[k] = tmp;
        }
    for (size_t i = 0; i < expect_count; ++i)
      for (size_t k = i + 1; k < expect_count; ++k)
        if (expect[k] > expect[i]) {
          const uint32_t tmp = expect[i];
          expect[i] = expect[k];
          expect[k] = tmp;
        }
    for (size_t i = 0; i < expect_count; ++i)
      multiset_ok &= got[i] == expect[i];
  }
  snprintf(what, sizeof(what), "records == crafted+appended (N=%zu g=%u)", N, g);
  tst_report(what, multiset_ok);

  free(expect);
  free(got);
  for (unsigned r = 0; r < tst_nrecords; ++r)
    free(tst_records[r]);
  tst_nrecords = 0;
  free(raw);
  tst_txn = nullptr;
}

__cold int mdbx_tst_gc_store_retired_cases(void) {
  /* goodchunk values below mirror the real ones for 4K pages
   * (maxgc_large1page - maxgc_large1page/16); the arithmetic under test is
   * goodchunk-relative, so any consistent pair exercises the same windows. */
  const size_t G = 955;  /* (3 mod 4): the endgame (left|3) branch hosts windows */
  const size_t GG = 958; /* (2 mod 4): the goodchunk branch hosts windows */

  static const size_t tst_small[] = {2, 3, 4, 5, 20, 100, 500};
  for (size_t s = 0; s < sizeof(tst_small) / sizeof(tst_small[0]); ++s)
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

  /* the goodchunk branch window (goodchunk (2 mod 4), N (goodchunk-2) */
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

  return (int)tst_failed_checks;
}

/* the dense solver lives in gc-put.c (MDBX_INTERNAL there); declared here
 * for the wrapper */
int gc_dense_pack(size_t slots, size_t volume, const pgno_t *hist_counts, unsigned hist_end, size_t first_page,
                  size_t other_pages, pgno_t *solution_counts, unsigned *solution_end, bool *found);

__cold int mdbx_tst_gc_dense_pack(size_t slots, size_t volume, const uint32_t *hist_counts, unsigned hist_end,
                                  size_t first_page, size_t other_pages, uint32_t *solution_counts,
                                  unsigned *solution_end, bool *found) {
  /* the histogram/solution element type is the internal pgno_t, pinned to
   * uint32_t by the STATIC_ASSERT above */
  return gc_dense_pack(slots, volume, (const pgno_t *)hist_counts, hist_end, first_page, other_pages,
                       (pgno_t *)solution_counts, solution_end, found);
}

/*----------------------------------------------------------------------------*/
/* The white-box migration seams (issue #519): read-only introspection of the
 * internal state and the build-configuration values; see tests/api.h. */

__cold int mdbx_tst_config_avoid_msync(void) { return MDBX_AVOID_MSYNC; }

__cold int mdbx_tst_config_mmap_needs_jolt(void) { return MDBX_MMAP_NEEDS_JOLT; }

__cold int mdbx_tst_config_exclusive_fastpath(void) { return MDBX_ENABLE_EXCLUSIVE_FASTPATH; }

__cold int mdbx_tst_env_mapping(const MDBX_env *env, void **base, size_t *limit, size_t *current) {
  if (unlikely(!env))
    return MDBX_EINVAL;
  if (base)
    *base = env->dxb_mmap.base;
  if (limit)
    *limit = env->dxb_mmap.limit;
  if (current)
    *current = env->dxb_mmap.current;
  return MDBX_SUCCESS;
}

__cold int mdbx_tst_dxb_grow(MDBX_env *env, uint64_t used_pgno, uint64_t size_pgno) {
  if (unlikely(!env || !env->dxb_mmap.base || used_pgno > size_pgno))
    return MDBX_EINVAL;
  const pgno_t limit_pgno = bytes2pgno(env, env->dxb_mmap.limit);
  if (unlikely(size_pgno > limit_pgno))
    return MDBX_EINVAL;
  return dxb_resize(env, (pgno_t)used_pgno, (pgno_t)size_pgno, limit_pgno, 0);
}

__cold uint64_t mdbx_tst_env_readahead_bytes(const MDBX_env *env) {
  if (unlikely(!env))
    return 0;
  /* the anchor packs the readahead enable-bit in its LSB and the edge page
   * number shifted left by one (see the dxb-resize.c readahead toggle) */
  return (uint64_t)(env->lck->readahead_anchor >> 1) * env->ps;
}

__cold int mdbx_tst_env_io_fds(const MDBX_env *env, mdbx_filehandle_t *overlapped_fd, mdbx_filehandle_t *dsync_fd,
                               mdbx_filehandle_t *fd4meta) {
  if (unlikely(!env))
    return MDBX_EINVAL;
#if IS_WINDOWS
  if (overlapped_fd)
    *overlapped_fd = env->ioring.overlapped_fd;
#else
  /* the overlapped i/o handle exists only in the Windows osal; reporting
   * the neutral zero keeps the seam cross-platform without cutting the API
   * off on the platform of the caller */
  if (overlapped_fd)
    *overlapped_fd = 0;
#endif /* Windows */
  if (dsync_fd)
    *dsync_fd = env->dsync_fd;
  if (fd4meta)
    *fd4meta = env->fd4meta;
  return MDBX_SUCCESS;
}

__cold int mdbx_tst_txn_loose_pages(const MDBX_txn *txn, size_t *counter, size_t *linked) {
  if (unlikely(!txn))
    return MDBX_EINVAL;
  if (counter)
    *counter = txn->wr.loose_count;
  if (linked) {
    /* the chain is threaded through the engine's page_next() link at the
     * start of the entries - the linkage the test-side hand replicas
     * duplicated */
    size_t count = 0;
    for (page_t *lp = txn->wr.loose_pages; lp; lp = page_next(lp))
      ++count;
    *linked = count;
  }
  return MDBX_SUCCESS;
}
