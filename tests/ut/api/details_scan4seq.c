/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.

/* White-box differential test of the SIMD scan4seq family (src/gc-scan4seq.h,
 * extracted verbatim from gc-get.c by the 2026-11 verification campaign).
 *
 * Space: ALL 2^16 strictly-descending lists over {1..16} with len >= 2,
 * every num in [2..len] (seq = num - 1), and multiple scratch placements
 * covering the page-safety classes of the SIMD tails. References:
 * scan4seq_fallback() == scan4range_checker() == a written-from-spec
 * brute force; every compiled SIMD variant (dispatched and called
 * directly when the CPU provides it) must return exactly the same
 * element pointer. Long-sequence spot checks cover seq far beyond the
 * 16-value universe. */

#include "../../../src/gc-scan4seq.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One might be tempted to re-implement the SIMD returns in the harness.
 * Do not: ptr_disp() displacements are in BYTES (utils.h:25), while the
 * x86 variants return via element arithmetic — mixing the units is the
 * exact trap that produced a false critical finding (issue #485,
 * V-002 retraction, docs/verification/v002-retraction.md). This test
 * calls the verbatim-extracted functions only. */

static pgno_t *scan4seq_bruteforce(pgno_t *range, const size_t len, const size_t seq) {
  pgno_t *detent = range - (ptrdiff_t)len + (ptrdiff_t)seq;
  const ptrdiff_t offset = -(ptrdiff_t)seq;
  const pgno_t target = (pgno_t)offset;
  for (pgno_t *p = range; p != detent; --p)
    if (*p - p[offset] == target)
      return p;
  return nullptr;
}

#define SCRATCH_SLOTS 16384u
#define PAD_SLOTS 64u /* SIMD tails may over-read up to a window; keep it inside the scratch */

static pgno_t scratch[SCRATCH_SLOTS + PAD_SLOTS];

static unsigned long n_cases, n_match, n_nomatch;
static unsigned long n_dives[5]; /* per-variant divergence counter */
static const char *const variant_names[5] = {"fallback", "impl(dispatch)", "sse2", "avx2", "avx512bw"};

static void check_case(pgno_t *pnl, size_t len, size_t seq, size_t num) {
  pgno_t *const edge = pnl + len;
  pgno_t *const refs[5] = {scan4seq_fallback(edge, len, seq), scan4seq_impl(edge, len, seq), nullptr, nullptr, nullptr};
  const pgno_t *const ck = scan4range_checker(pnl, seq);
  pgno_t *const brute = scan4seq_bruteforce(edge, len, seq);
  /* Not assert(): under -DNDEBUG the references must still be checked, and
   * the locals must stay used for the -Werror CI cells. */
  const int mismatch =
      (refs[0] != brute) || ((ck == nullptr) != (refs[0] == nullptr)) || (ck && refs[0] && ck - pnl != refs[0] - pnl);
  if (mismatch) {
    n_dives[0] += 1;
    printf("MISMATCH reference: len=%zu num=%zu seq=%zu fallback=%ld brute=%ld ck=%ld\n", len, num, seq,
           (long)(refs[0] - pnl), (long)(brute - pnl), (long)(ck - pnl));
  }
  n_cases += 1;
  if (refs[0])
    n_match += 1;
  else
    n_nomatch += 1;
  /* the dispatch target, and every variant the CPU provides, one by one */
  const pgno_t *const alts[3] = {
/* A variant is callable directly only when the compiler offers the CPU
 * feature probe (MDBX_HAVE_BUILTIN_CPU_SUPPORTS, the resolver's own gate):
 * e.g. MSVC compiles the sse2 variant in (MDBX_ATTRIBUTE_TARGET_* expand to
 * nothing there) but has no __builtin_cpu_supports(). */
#if defined(MDBX_ATTRIBUTE_TARGET_SSE2) && MDBX_HAVE_BUILTIN_CPU_SUPPORTS
      __builtin_cpu_supports("sse2") ? scan4seq_sse2(edge, len, seq) : nullptr,
#else
      nullptr,
#endif
#if defined(MDBX_ATTRIBUTE_TARGET_AVX2) && MDBX_HAVE_BUILTIN_CPU_SUPPORTS
      (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("sse2")) ? scan4seq_avx2(edge, len, seq) : nullptr,
#else
      nullptr,
#endif
#if defined(MDBX_ATTRIBUTE_TARGET_AVX512BW) && MDBX_HAVE_BUILTIN_CPU_SUPPORTS
      (__builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx2")) ? scan4seq_avx512bw(edge, len, seq)
                                                                             : nullptr,
#else
      nullptr
#endif
  };
  for (unsigned i = 1; i < 5; ++i) {
    const pgno_t *res = (i == 1) ? refs[1] : alts[i - 2];
    if (i >= 2 && !alts[i - 2])
      continue; /* variant not provided by the CPU */
    if (res != refs[0]) {
      n_dives[i] += 1;
      printf("MISMATCH %s: len=%zu num=%zu seq=%zu fallback=%ld variant=%ld\n", variant_names[i], len, num, seq,
             (long)(refs[0] - pnl), res ? (long)(res - pnl) : -9999);
    }
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
        pgno_t *const raw = scratch + offsets[off_i];
        off_i = (off_i + 1) % noffs;
        pgno_t *const pnl = raw + 1;
        raw[0] = (pgno_t)(len + 1);
        pnl[0] = (pgno_t)len;
        size_t k = 1;
        for (int v = 16; v >= 1; --v)
          if (set & (1u << (v - 1)))
            pnl[k++] = (pgno_t)v;
        assert(k == len + 1);
        check_case(pnl, len, seq, num);
      }
    }
  }
}

static void run_longseq(void) {
#define LONG_LEN 1200
  static pgno_t raw[LONG_LEN + 2 + PAD_SLOTS];
  pgno_t *const pnl = raw + 1;
  unsigned long checks = 0;
  for (size_t trial = 0; trial < 170; ++trial) {
    const size_t base_seq = 1 + trial * 7;
    pgno_t v = 3000u + (pgno_t)(base_seq * 4);
    for (size_t i = LONG_LEN; i >= 1; --i) {
      pnl[i] = v;
      if (v > 1)
        v -= 3;
    }
    for (size_t idx = 600 - base_seq; idx <= 600; ++idx)
      pnl[idx] = (pgno_t)(5000 + (idx - (600 - base_seq)));
    pnl[0] = (pgno_t)LONG_LEN;
    pgno_t *const edge = pnl + LONG_LEN;
    const size_t nums[] = {2, 3, 5, 8, 40, base_seq, base_seq + 1, 1000};
    for (unsigned ni = 0; ni < 8; ++ni) {
      const size_t num = nums[ni];
      if (num < 2 || num > LONG_LEN)
        continue;
      const size_t seq = num - 1;
      pgno_t *const fb = scan4seq_fallback(edge, LONG_LEN, seq);
      pgno_t *const impl = scan4seq_impl(edge, LONG_LEN, seq);
      const pgno_t *const ck = scan4range_checker(pnl, seq);
      checks += 1;
      const int mismatch = (fb != impl) || ((ck == nullptr) != (fb == nullptr)) || (ck && fb && ck - pnl != fb - pnl);
      if (mismatch) {
        n_dives[1] += 1;
        printf("MISMATCH impl(long): trial=%zu num=%zu seq=%zu fallback=%ld impl=%ld\n", trial, num, seq,
               (long)(fb - pnl), (long)(impl - pnl));
      }
    }
  }
  n_cases += checks;
  printf("long-seq: %lu checks\n", checks);
}

int main(void) {
  memset(scratch, 0xAA, sizeof(scratch)); /* poison: junk lanes must never be selected */
  run_exhaustive();
  run_longseq();
  printf("cases=%lu (matches=%lu nomatch=%lu)\n", n_cases, n_match, n_nomatch);
  for (unsigned i = 0; i < 5; ++i)
    printf("%-16s divergences: %lu\n", variant_names[i], n_dives[i]);
  const unsigned long total = n_dives[0] + n_dives[1] + n_dives[2] + n_dives[3] + n_dives[4];
  if (total) {
    printf("FAILED: %lu divergences\n", total);
    return 1;
  }
  printf("PASS: all variants agree with the references\n");
  return 0;
}
