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

/*----------------------------------------------------------------------------*/

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
