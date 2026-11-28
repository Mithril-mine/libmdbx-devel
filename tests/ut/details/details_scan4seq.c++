/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.

/* Differential test of the SIMD scan4seq family (src/gc-scan4seq.h,
 * extracted verbatim from gc-get.c by the 2026-11 verification campaign),
 * driven through the testing API (mdbx_tst_scan4seq_*, see tests/api.h).
 *
 * Space: ALL 2^16 strictly-descending lists over {1..16} with len >= 2,
 * every num in [2..len] (seq = num - 1), and multiple scratch placements
 * covering the page-safety classes of the SIMD tails. References:
 * scan4seq_fallback() == the internal scan4range_checker() == a
 * written-from-spec brute force; every compiled SIMD variant (dispatched
 * and called directly when the CPU provides it) must return exactly the
 * same element pointer. Long-sequence spot checks cover seq far beyond
 * the 16-value universe. */

#include "mdbx.h"
#include <gtest/gtest.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* One might be tempted to re-implement the SIMD returns in the harness.
 * Do not: the x86 variants return via element arithmetic while the
 * checker's displacements are in BYTES - mixing the units is the exact
 * trap that produced a false critical finding (issue #485, scan4seq-units-false-claim
 * retraction, docs/verification/v002-retraction.md). This test drives the
 * verbatim library code through the testing API wrappers. */

/* The list element type, pinned to the internal pgno_t by the testing
 * API's STATIC_ASSERT (see tests/api.c). */
typedef uint32_t page_no;

/* The written-from-spec oracle: a plain backwards loop independent of any
 * implementation detail of the SIMD family. */
static page_no *scan4seq_bruteforce(page_no *range, const size_t len, const size_t seq) {
  page_no *detent = range - (ptrdiff_t)len + (ptrdiff_t)seq;
  const ptrdiff_t offset = -(ptrdiff_t)seq;
  const page_no target = (page_no)offset;
  for (page_no *p = range; p != detent; --p)
    if (*p - p[offset] == target)
      return p;
  return NULL;
}

/* The written-from-spec oracle for the exact-length variant: walk the maximal
 * descending-consecutive runs from the tail upward and take the first one of
 * exactly seq+1 pages; a longer run is skipped as a whole (it must stay
 * available to a larger request), a shorter one cannot qualify. */
static page_no *scan4seq_exact_bruteforce(page_no *range, const size_t len, const size_t seq) {
  page_no *const pnl = range - (ptrdiff_t)len;
  const size_t num = seq + 1;
  for (size_t end = len; end >= 1;) {
    size_t top = end;
    while (top > 1 && pnl[top - 1] == pnl[top] + 1)
      --top;
    if (end - top + 1 == num)
      return pnl + end;
    if (top <= 1)
      break;
    end = top - 1;
  }
  return NULL;
}

#define SCRATCH_SLOTS 16384u
#define PAD_SLOTS 64u /* SIMD tails may over-read up to a window; keep it inside the scratch */

static page_no scratch[SCRATCH_SLOTS + PAD_SLOTS];

static unsigned long n_cases, n_match, n_nomatch;
static unsigned long n_dives[5]; /* per-variant divergence counter */
static unsigned long n_dives_exact;
static const char *const variant_names[5] = {"fallback", "impl(dispatch)", "sse2", "avx2", "avx512bw"};

/* The CI shuffles the TEST order (GTEST_SHUFFLE=1), so every TEST resets the
 * counters itself; a divergence reports through ADD_FAILURE and is counted
 * for the per-TEST summary. */
static void scan4seq_reset(void) {
  n_cases = n_match = n_nomatch = 0;
  memset(n_dives, 0, sizeof(n_dives));
  n_dives_exact = 0;
}

static void check_case(page_no *pnl, size_t len, size_t seq, size_t num) {
  page_no *const edge = pnl + len;
  page_no *const refs[5] = {mdbx_tst_scan4seq_variant(MDBX_TST_SCAN4SEQ_FALLBACK, edge, len, seq),
                            mdbx_tst_scan4seq_variant(MDBX_TST_SCAN4SEQ_IMPL, edge, len, seq),
                            (mdbx_tst_scan4seq_variants_mask() & (1u << MDBX_TST_SCAN4SEQ_SSE2))
                                ? mdbx_tst_scan4seq_variant(MDBX_TST_SCAN4SEQ_SSE2, edge, len, seq)
                                : NULL,
                            (mdbx_tst_scan4seq_variants_mask() & (1u << MDBX_TST_SCAN4SEQ_AVX2))
                                ? mdbx_tst_scan4seq_variant(MDBX_TST_SCAN4SEQ_AVX2, edge, len, seq)
                                : NULL,
                            (mdbx_tst_scan4seq_variants_mask() & (1u << MDBX_TST_SCAN4SEQ_AVX512BW))
                                ? mdbx_tst_scan4seq_variant(MDBX_TST_SCAN4SEQ_AVX512BW, edge, len, seq)
                                : NULL};
  const page_no *const ck = mdbx_tst_scan4seq_checker(pnl, seq);
  page_no *const brute = scan4seq_bruteforce(edge, len, seq);
  /* Not assert(): under -DNDEBUG the references must still be checked, and
   * the locals must stay used for the -Werror CI cells. */
  const int mismatch =
      (refs[0] != brute) || ((ck == NULL) != (refs[0] == NULL)) || (ck && refs[0] && ck - pnl != refs[0] - pnl);
  if (mismatch) {
    n_dives[0] += 1;
    ADD_FAILURE() << "MISMATCH reference: len=" << len << " num=" << num << " seq=" << seq
                  << " fallback=" << (refs[0] - pnl) << " brute=" << (brute - pnl) << " ck=" << (ck ? ck - pnl : -9999);
  }
  n_cases += 1;
  if (refs[0])
    n_match += 1;
  else
    n_nomatch += 1;
  /* the dispatch target, and every variant the CPU provides, one by one */
  for (unsigned i = 1; i < 5; ++i) {
    if (i >= 2 && !refs[i])
      continue; /* variant not provided by the CPU/build */
    if (refs[i] != refs[0]) {
      n_dives[i] += 1;
      ADD_FAILURE() << "MISMATCH " << variant_names[i] << ": len=" << len << " num=" << num << " seq=" << seq
                    << " fallback=" << (refs[0] - pnl) << " variant=" << (refs[i] ? refs[i] - pnl : -9999);
    }
  }
  /* the exact-length variant: only sequences of exactly num pages qualify */
  const page_no *const ex = mdbx_tst_scan4seq_exact(pnl, seq);
  page_no *const ex_brute = scan4seq_exact_bruteforce(edge, len, seq);
  if (ex != ex_brute) {
    n_dives_exact += 1;
    ADD_FAILURE() << "MISMATCH exact: len=" << len << " num=" << num << " seq=" << seq
                  << " exact=" << (ex ? ex - pnl : -9999) << " brute=" << (ex_brute ? ex_brute - pnl : -9999);
  }
}

/* Not __builtin_popcount(): MSVC has no such builtin, and the 16-bit
 * universe needs nothing fancier than Kernighan's loop. */
static unsigned popcount16(unsigned value) {
  unsigned count = 0;
  for (; value; value &= value - 1)
    count += 1;
  return count;
}

static void run_exhaustive(void) {
  const unsigned offsets[] = {0x000u, 0x020u, 0x040u, 0x0f0u, 0x100u, 0x1f0u, 0x200u, 0x3f0u,
                              0x400u, 0x5f0u, 0x7f0u, 0x800u, 0xbf0u, 0xc00u, 0xdf0u, 0xff0u};
  const unsigned noffs = sizeof(offsets) / sizeof(offsets[0]);
  unsigned off_i = 0;
  for (uint32_t set = 0; set < 65536u; ++set) {
    const size_t len = (size_t)popcount16(set);
    if (len < 2 || len + 1 + PAD_SLOTS > SCRATCH_SLOTS)
      continue;
    for (size_t num = 2; num <= len; ++num) {
      const size_t seq = num - 1;
      for (unsigned mode = 0; mode < 2; ++mode) {
        page_no *const raw = scratch + offsets[off_i];
        off_i = (off_i + 1) % noffs;
        page_no *const pnl = raw + 1;
        raw[0] = (page_no)(len + 1);
        pnl[0] = (page_no)len;
        size_t k = 1;
        for (int v = 16; v >= 1; --v)
          if (set & (1u << (v - 1)))
            pnl[k++] = (page_no)v;
        assert(k == len + 1);
        check_case(pnl, len, seq, num);
      }
    }
  }
}

static void run_longseq(void) {
#define LONG_LEN 1200
  static page_no raw[LONG_LEN + 2 + PAD_SLOTS];
  page_no *const pnl = raw + 1;
  unsigned long checks = 0;
  for (size_t trial = 0; trial < 170; ++trial) {
    const size_t base_seq = 1 + trial * 7;
    page_no v = 3000u + (page_no)(base_seq * 4);
    for (size_t i = LONG_LEN; i >= 1; --i) {
      pnl[i] = v;
      if (v > 1)
        v -= 3;
    }
    for (size_t idx = 600 - base_seq; idx <= 600; ++idx)
      pnl[idx] = (page_no)(5000 + (idx - (600 - base_seq)));
    pnl[0] = (page_no)LONG_LEN;
    page_no *const edge = pnl + LONG_LEN;
    const size_t nums[] = {2, 3, 5, 8, 40, base_seq, base_seq + 1, 1000};
    for (unsigned ni = 0; ni < 8; ++ni) {
      const size_t num = nums[ni];
      if (num < 2 || num > LONG_LEN)
        continue;
      const size_t seq = num - 1;
      page_no *const fb = mdbx_tst_scan4seq_variant(MDBX_TST_SCAN4SEQ_FALLBACK, edge, LONG_LEN, seq);
      page_no *const impl = mdbx_tst_scan4seq_variant(MDBX_TST_SCAN4SEQ_IMPL, edge, LONG_LEN, seq);
      const page_no *const ck = mdbx_tst_scan4seq_checker(pnl, seq);
      checks += 1;
      const int mismatch = (fb != impl) || ((ck == NULL) != (fb == NULL)) || (ck && fb && ck - pnl != fb - pnl);
      if (mismatch) {
        n_dives[1] += 1;
        ADD_FAILURE() << "MISMATCH impl(long): trial=" << trial << " num=" << num << " seq=" << seq
                      << " fallback=" << (fb - pnl) << " impl=" << (impl - pnl);
      }
    }
  }
  n_cases += checks;
  printf("long-seq: %lu checks\n", checks);
}

/* Long-list counterpart of the exact-length check: a strictly descending list
 * of runs of 2..8 consecutive pages separated by gaps, so a request either
 * matches one of the runs exactly or has no sequence at all. The oracle is the
 * independent written-from-spec walk above, not a copy of the library code. */
static void run_exact_long(void) {
  enum { EXACT_LEN = 1200 };
  static page_no raw[EXACT_LEN + 1 + PAD_SLOTS];
  page_no *const pnl = raw + 1;
  unsigned long checks = 0;
  for (size_t trial = 0; trial < 40; ++trial) {
    page_no next = (page_no)(200000 + trial * 1000);
    size_t i = 1, k = 0;
    while (i <= EXACT_LEN) {
      const size_t run_len = 2 + (trial + k) % 7; /* 2..8 consecutive pages */
      for (size_t j = 0; j < run_len && i <= EXACT_LEN; ++j, ++i)
        pnl[i] = next--;
      next -= 3; /* a gap, so the next run is a separate sequence */
      k += 1;
    }
    pnl[0] = (page_no)EXACT_LEN;
    for (size_t idx = 1; idx < EXACT_LEN; ++idx)
      if (pnl[idx] <= pnl[idx + 1]) {
        n_dives_exact += 1;
        ADD_FAILURE() << "MISMATCH exact(long): trial=" << trial << " not strictly descending at " << idx;
        break;
      }
    const size_t nums[] = {2, 3, 5, 8, 9, 40, 1000};
    for (unsigned ni = 0; ni < sizeof(nums) / sizeof(nums[0]); ++ni) {
      const size_t num = nums[ni];
      const size_t seq = num - 1;
      const page_no *const ex = mdbx_tst_scan4seq_exact(pnl, seq);
      page_no *const brute = scan4seq_exact_bruteforce(pnl + EXACT_LEN, EXACT_LEN, seq);
      checks += 1;
      if (ex != brute) {
        n_dives_exact += 1;
        ADD_FAILURE() << "MISMATCH exact(long): trial=" << trial << " num=" << num << " seq=" << seq
                      << " exact=" << (ex ? ex - pnl : -9999) << " brute=" << (brute ? brute - pnl : -9999);
      }
    }
  }
  n_cases += checks;
  printf("exact long-seq: %lu checks\n", checks);
}

static unsigned long dive_total(void) {
  return n_dives[0] + n_dives[1] + n_dives[2] + n_dives[3] + n_dives[4] + n_dives_exact;
}

TEST(details_scan4seq, exhaustive) {
  scan4seq_reset();
  memset(scratch, 0xAA, sizeof(scratch)); /* poison: junk lanes must never be selected */
  run_exhaustive();
  printf("cases=%lu (matches=%lu nomatch=%lu)\n", n_cases, n_match, n_nomatch);
  EXPECT_EQ(0u, dive_total()) << "the exhaustive sweep must agree with the references";
}

TEST(details_scan4seq, longseq) {
  scan4seq_reset();
  run_longseq();
  EXPECT_EQ(0u, dive_total()) << "the long-sequence spot checks must agree with the references";
}

TEST(details_scan4seq, exact_long) {
  scan4seq_reset();
  run_exact_long();
  EXPECT_EQ(0u, dive_total()) << "the exact-length long checks must agree with the references";
}
