/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change, credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

/** \file gc-scan4seq.h — SIMD search for a contiguous descending run (sequence of
 * `seq+1` pages) within a page-number list. Extracted verbatim from gc-get.c
 * (2026-11, verification campaign) to allow standalone white-box testing
 * (tests/ut/details_scan4seq.c). Requires essentials.h (pgno_t, ptr_disp,
 * clz_uintptr, ASSERT, arm_neon.h/immintrin via preface.h) and pnl.h. */

#include "essentials.h"

MDBX_MAYBE_UNUSED __hot static pgno_t *scan4seq_fallback(pgno_t *range, const size_t len, const size_t seq) {
  ASSERT(seq > 0 && len > seq);
#if MDBX_PNL_ASCENDING
  ASSERT(range[-1] == len);
  const pgno_t *const detent = range + len - seq;
  const ptrdiff_t offset = (ptrdiff_t)seq;
  const pgno_t target = (pgno_t)offset;
  if (likely(len > seq + 3)) {
    do {
      const pgno_t diff0 = range[offset + 0] - range[0];
      const pgno_t diff1 = range[offset + 1] - range[1];
      const pgno_t diff2 = range[offset + 2] - range[2];
      const pgno_t diff3 = range[offset + 3] - range[3];
      if (diff0 == target)
        return range + 0;
      if (diff1 == target)
        return range + 1;
      if (diff2 == target)
        return range + 2;
      if (diff3 == target)
        return range + 3;
      range += 4;
    } while (range + 3 < detent);
    if (range == detent)
      return nullptr;
  }
  do
    if (range[offset] - *range == target)
      return range;
  while (++range < detent);
#else
  ASSERT(range[-(ptrdiff_t)len] == len);
  const pgno_t *const detent = range - len + seq;
  const ptrdiff_t offset = -(ptrdiff_t)seq;
  const pgno_t target = (pgno_t)offset;
  if (likely(len > seq + 3)) {
    do {
      const pgno_t diff0 = range[-0] - range[offset - 0];
      const pgno_t diff1 = range[-1] - range[offset - 1];
      const pgno_t diff2 = range[-2] - range[offset - 2];
      const pgno_t diff3 = range[-3] - range[offset - 3];
      /* Смысл вычислений до ветвлений в том, чтобы позволить компилятору
       * загружать и вычислять все значения параллельно. */
      if (diff0 == target)
        return range - 0;
      if (diff1 == target)
        return range - 1;
      if (diff2 == target)
        return range - 2;
      if (diff3 == target)
        return range - 3;
      range -= 4;
    } while (range > detent + 3);
    if (range == detent)
      return nullptr;
  }
  do
    if (*range - range[offset] == target)
      return range;
  while (--range > detent);
#endif /* pnl_t sort-order */
  return nullptr;
}

MDBX_MAYBE_UNUSED static const pgno_t *scan4range_checker(const pnl_t pnl, const size_t seq) {
  size_t begin = MDBX_PNL_ASCENDING ? 1 : pnl_size(pnl);
#if MDBX_PNL_ASCENDING
  while (seq <= pnl_size(pnl) - begin) {
    if (pnl[begin + seq] - pnl[begin] == seq)
      return pnl + begin;
    ++begin;
  }
#else
  while (begin > seq) {
    if (pnl[begin - seq] - pnl[begin] == seq)
      return pnl + begin;
    --begin;
  }
#endif /* pnl_t sort-order */
  return nullptr;
}

#if !MDBX_PNL_ASCENDING

#if !defined(MDBX_ATTRIBUTE_TARGET) && (__has_attribute(__target__) || __GNUC_PREREQ(5, 0))
#define MDBX_ATTRIBUTE_TARGET(target) __attribute__((__target__(target)))
#endif /* MDBX_ATTRIBUTE_TARGET */

#ifndef MDBX_GCC_FASTMATH_i686_SIMD_WORKAROUND
/* Workaround for GCC's bug with `-m32 -march=i686 -Ofast`
 * gcc/i686-buildroot-linux-gnu/12.2.0/include/xmmintrin.h:814:1:
 *     error: inlining failed in call to 'always_inline' '_mm_movemask_ps':
 *            target specific option mismatch */
#if !defined(__FAST_MATH__) || !__FAST_MATH__ || !defined(__GNUC__) || defined(__e2k__) || defined(__clang__) ||       \
    defined(__amd64__) || defined(__SSE2__)
#define MDBX_GCC_FASTMATH_i686_SIMD_WORKAROUND 0
#else
#define MDBX_GCC_FASTMATH_i686_SIMD_WORKAROUND 1
#endif
#endif /* MDBX_GCC_FASTMATH_i686_SIMD_WORKAROUND */

#if defined(__SSE2__) && defined(__SSE__)
#define MDBX_ATTRIBUTE_TARGET_SSE2 /* nope */
#elif (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(__amd64__)
#define __SSE2__
#define MDBX_ATTRIBUTE_TARGET_SSE2 /* nope */
#elif defined(MDBX_ATTRIBUTE_TARGET) && defined(__ia32__) && !MDBX_GCC_FASTMATH_i686_SIMD_WORKAROUND
#define MDBX_ATTRIBUTE_TARGET_SSE2 MDBX_ATTRIBUTE_TARGET("sse,sse2")
#endif /* __SSE2__ */

#if defined(__AVX2__)
#define MDBX_ATTRIBUTE_TARGET_AVX2 /* nope */
#elif defined(MDBX_ATTRIBUTE_TARGET) && defined(__ia32__) && !MDBX_GCC_FASTMATH_i686_SIMD_WORKAROUND
#define MDBX_ATTRIBUTE_TARGET_AVX2 MDBX_ATTRIBUTE_TARGET("sse,sse2,avx,avx2")
#endif /* __AVX2__ */

#if defined(MDBX_ATTRIBUTE_TARGET_AVX2)
#if defined(__AVX512BW__)
#define MDBX_ATTRIBUTE_TARGET_AVX512BW /* nope */
#elif defined(MDBX_ATTRIBUTE_TARGET) && defined(__ia32__) && !MDBX_GCC_FASTMATH_i686_SIMD_WORKAROUND &&                \
    (__GNUC_PREREQ(6, 0) || __CLANG_PREREQ(5, 0))
#define MDBX_ATTRIBUTE_TARGET_AVX512BW MDBX_ATTRIBUTE_TARGET("sse,sse2,avx,avx2,avx512bw")
#endif /* __AVX512BW__ */
#endif /* MDBX_ATTRIBUTE_TARGET_AVX2 for MDBX_ATTRIBUTE_TARGET_AVX512BW */

#ifdef MDBX_ATTRIBUTE_TARGET_SSE2
MDBX_ATTRIBUTE_TARGET_SSE2 static __always_inline unsigned
diffcmp2mask_sse2(const pgno_t *const ptr, const ptrdiff_t offset, const __m128i pattern) {
  const __m128i f = _mm_loadu_si128((const __m128i *)ptr);
  const __m128i l = _mm_loadu_si128((const __m128i *)(ptr + offset));
  const __m128i cmp = _mm_cmpeq_epi32(_mm_sub_epi32(f, l), pattern);
  return _mm_movemask_ps(*(const __m128 *)&cmp);
}

MDBX_MAYBE_UNUSED __hot MDBX_ATTRIBUTE_TARGET_SSE2 static pgno_t *scan4seq_sse2(pgno_t *range, const size_t len,
                                                                                const size_t seq) {
  ASSERT(seq > 0 && len > seq);
#if MDBX_PNL_ASCENDING
#error "FIXME: Not implemented"
#endif /* MDBX_PNL_ASCENDING */
  ASSERT(range[-(ptrdiff_t)len] == len);
  pgno_t *const detent = range - len + seq;
  const ptrdiff_t offset = -(ptrdiff_t)seq;
  const pgno_t target = (pgno_t)offset;
  const __m128i pattern = _mm_set1_epi32(target);
  uint8_t mask;
  if (likely(len > seq + 3)) {
    do {
      mask = (uint8_t)diffcmp2mask_sse2(range - 3, offset, pattern);
      if (mask) {
#if !defined(ENABLE_MEMCHECK) && !defined(__SANITIZE_ADDRESS__)
      found:;
#endif /* !ENABLE_MEMCHECK && !__SANITIZE_ADDRESS__ */
        const unsigned clz_bits = (unsigned)(sizeof(unsigned) * CHAR_BIT);
        const unsigned sse2_lanes = 4;
        const unsigned clz_bias = clz_bits - sse2_lanes; /* 32 - 4 = 28 for 32-bit unsigned */
        return range + clz_bias - clz32(mask);
      }
      range -= 4;
    } while (range > detent + 3);
    if (range == detent)
      return nullptr;
  }

  /* Далее происходит чтение от 4 до 12 лишних байт, которые могут быть не
   * только за пределами региона выделенного под PNL, но и пересекать границу
   * страницы памяти. Что может приводить как к ошибкам ASAN, так и к падению.
   * Поэтому проверяем смещение на странице, а с ASAN всегда страхуемся. */
#if !defined(ENABLE_MEMCHECK) && !defined(__SANITIZE_ADDRESS__)
  const unsigned on_page_safe_mask = 0xff0 /* enough for '-15' bytes offset */;
  if (likely(on_page_safe_mask & (uintptr_t)(range + offset)) && !RUNNING_ON_VALGRIND) {
    const unsigned extra = (unsigned)(detent + 4 - range);
    ASSERT(extra > 0 && extra < 4);
    mask = 0xFu << extra;
    mask &= diffcmp2mask_sse2(range - 3, offset, pattern);
    if (mask)
      goto found;
    return nullptr;
  }
#endif /* !ENABLE_MEMCHECK && !__SANITIZE_ADDRESS__ */
  do
    if (*range - range[offset] == target)
      return range;
  while (--range != detent);
  return nullptr;
}
#endif /* MDBX_ATTRIBUTE_TARGET_SSE2 */

#ifdef MDBX_ATTRIBUTE_TARGET_AVX2
MDBX_ATTRIBUTE_TARGET_AVX2 static __always_inline unsigned
diffcmp2mask_avx2(const pgno_t *const ptr, const ptrdiff_t offset, const __m256i pattern) {
  const __m256i f = _mm256_loadu_si256((const __m256i *)ptr);
  const __m256i l = _mm256_loadu_si256((const __m256i *)(ptr + offset));
  const __m256i cmp = _mm256_cmpeq_epi32(_mm256_sub_epi32(f, l), pattern);
  return _mm256_movemask_ps(*(const __m256 *)&cmp);
}

MDBX_ATTRIBUTE_TARGET_AVX2 static __always_inline unsigned
diffcmp2mask_sse2avx(const pgno_t *const ptr, const ptrdiff_t offset, const __m128i pattern) {
  const __m128i f = _mm_loadu_si128((const __m128i *)ptr);
  const __m128i l = _mm_loadu_si128((const __m128i *)(ptr + offset));
  const __m128i cmp = _mm_cmpeq_epi32(_mm_sub_epi32(f, l), pattern);
  return _mm_movemask_ps(*(const __m128 *)&cmp);
}

MDBX_MAYBE_UNUSED __hot MDBX_ATTRIBUTE_TARGET_AVX2 static pgno_t *scan4seq_avx2(pgno_t *range, const size_t len,
                                                                                const size_t seq) {
  ASSERT(seq > 0 && len > seq);
#if MDBX_PNL_ASCENDING
#error "FIXME: Since 2026-04-01 alternatives to MDBX_PNL_ASCENDING = 0 are no longer supported."
#endif /* MDBX_PNL_ASCENDING */
  ASSERT(range[-(ptrdiff_t)len] == len);
  pgno_t *const detent = range - len + seq;
  const ptrdiff_t offset = -(ptrdiff_t)seq;
  const pgno_t target = (pgno_t)offset;
  const __m256i pattern = _mm256_set1_epi32(target);
  uint8_t mask;
  if (likely(len > seq + 7)) {
    do {
      mask = (uint8_t)diffcmp2mask_avx2(range - 7, offset, pattern);
      if (mask) {
#if !defined(ENABLE_MEMCHECK) && !defined(__SANITIZE_ADDRESS__)
      found:
#endif /* !ENABLE_MEMCHECK && !__SANITIZE_ADDRESS__ */
        return range + 24 - clz32(mask);
      }
      range -= 8;
    } while (range > detent + 7);
    if (range == detent)
      return nullptr;
  }

  /* Далее происходит чтение от 4 до 28 лишних байт, которые могут быть не
   * только за пределами региона выделенного под PNL, но и пересекать границу
   * страницы памяти. Что может приводить как к ошибкам ASAN, так и к падению.
   * Поэтому проверяем смещение на странице, а с ASAN всегда страхуемся. */
#if !defined(ENABLE_MEMCHECK) && !defined(__SANITIZE_ADDRESS__)
  const unsigned on_page_safe_mask = 0xfe0 /* enough for '-31' bytes offset */;
  if (likely(on_page_safe_mask & (uintptr_t)(range + offset)) && !RUNNING_ON_VALGRIND) {
    const unsigned extra = (unsigned)(detent + 8 - range);
    ASSERT(extra > 0 && extra < 8);
    mask = 0xFF << extra;
    mask &= diffcmp2mask_avx2(range - 7, offset, pattern);
    if (mask)
      goto found;
    return nullptr;
  }
#endif /* !ENABLE_MEMCHECK && !__SANITIZE_ADDRESS__ */
  if (range - 3 > detent) {
    mask = diffcmp2mask_sse2avx(range - 3, offset, *(const __m128i *)&pattern);
    if (mask)
      return range + 28 - clz32(mask);
    range -= 4;
  }
  while (range > detent) {
    if (*range - range[offset] == target)
      return range;
    --range;
  }
  return nullptr;
}
#endif /* MDBX_ATTRIBUTE_TARGET_AVX2 */

#ifdef MDBX_ATTRIBUTE_TARGET_AVX512BW
MDBX_ATTRIBUTE_TARGET_AVX512BW static __always_inline unsigned
diffcmp2mask_avx512bw(const pgno_t *const ptr, const ptrdiff_t offset, const __m512i pattern) {
  const __m512i f = _mm512_loadu_si512((const __m512i *)ptr);
  const __m512i l = _mm512_loadu_si512((const __m512i *)(ptr + offset));
  return _mm512_cmpeq_epi32_mask(_mm512_sub_epi32(f, l), pattern);
}

MDBX_MAYBE_UNUSED __hot MDBX_ATTRIBUTE_TARGET_AVX512BW static pgno_t *scan4seq_avx512bw(pgno_t *range, const size_t len,
                                                                                        const size_t seq) {
  ASSERT(seq > 0 && len > seq);
#if MDBX_PNL_ASCENDING
#error "FIXME: Since 2026-04-01 alternatives to MDBX_PNL_ASCENDING = 0 are no longer supported."
#endif /* MDBX_PNL_ASCENDING */
  ASSERT(range[-(ptrdiff_t)len] == len);
  pgno_t *const detent = range - len + seq;
  const ptrdiff_t offset = -(ptrdiff_t)seq;
  const pgno_t target = (pgno_t)offset;
  const __m512i pattern = _mm512_set1_epi32(target);
  unsigned mask;
  if (likely(len > seq + 15)) {
    do {
      mask = diffcmp2mask_avx512bw(range - 15, offset, pattern);
      if (mask) {
#if !defined(ENABLE_MEMCHECK) && !defined(__SANITIZE_ADDRESS__)
      found:
#endif /* !ENABLE_MEMCHECK && !__SANITIZE_ADDRESS__ */
        return range + 16 - clz32(mask);
      }
      range -= 16;
    } while (range > detent + 15);
    if (range == detent)
      return nullptr;
  }

  /* Далее происходит чтение от 4 до 60 лишних байт, которые могут быть не
   * только за пределами региона выделенного под PNL, но и пересекать границу
   * страницы памяти. Что может приводить как к ошибкам ASAN, так и к падению.
   * Поэтому проверяем смещение на странице, а с ASAN всегда страхуемся. */
#if !defined(ENABLE_MEMCHECK) && !defined(__SANITIZE_ADDRESS__)
  const unsigned on_page_safe_mask = 0xfc0 /* enough for '-63' bytes offset */;
  if (likely(on_page_safe_mask & (uintptr_t)(range + offset)) && !RUNNING_ON_VALGRIND) {
    const unsigned extra = (unsigned)(detent + 16 - range);
    ASSERT(extra > 0 && extra < 16);
    mask = 0xFFFFu << extra;
    mask &= diffcmp2mask_avx512bw(range - 15, offset, pattern);
    if (mask)
      goto found;
    return nullptr;
  }
#endif /* !ENABLE_MEMCHECK && !__SANITIZE_ADDRESS__ */
  if (range - 7 > detent) {
    mask = diffcmp2mask_avx2(range - 7, offset, *(const __m256i *)&pattern);
    if (mask)
      return range + 24 - clz32(mask);
    range -= 8;
  }
  if (range - 3 > detent) {
    mask = diffcmp2mask_sse2avx(range - 3, offset, *(const __m128i *)&pattern);
    if (mask)
      return range + 28 - clz32(mask);
    range -= 4;
  }
  while (range > detent) {
    if (*range - range[offset] == target)
      return range;
    --range;
  }
  return nullptr;
}
#endif /* MDBX_ATTRIBUTE_TARGET_AVX512BW */

#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
static __always_inline size_t diffcmp2mask_neon(const pgno_t *const ptr, const ptrdiff_t offset,
                                                const uint32x4_t pattern) {
  const uint32x4_t f = vld1q_u32(ptr);
  const uint32x4_t l = vld1q_u32(ptr + offset);
  const uint16x4_t cmp = vmovn_u32(vceqq_u32(vsubq_u32(f, l), pattern));
  if (sizeof(size_t) >= 8)
    return vget_lane_u64(vreinterpret_u64_u16(cmp), 0);
  else
    return vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(cmp, cmp))), 0);
}

__hot static pgno_t *scan4seq_neon(pgno_t *range, const size_t len, const size_t seq) {
  ASSERT(seq > 0 && len > seq);
#if MDBX_PNL_ASCENDING
#error "FIXME: Since 2026-04-01 alternatives to MDBX_PNL_ASCENDING = 0 are no longer supported."
#endif /* MDBX_PNL_ASCENDING */
  ASSERT(range[-(ptrdiff_t)len] == len);
  pgno_t *const detent = range - len + seq;
  const ptrdiff_t offset = -(ptrdiff_t)seq;
  const pgno_t target = (pgno_t)offset;
  const uint32x4_t pattern = vmovq_n_u32(target);
  size_t mask;
  if (likely(len > seq + 3)) {
    do {
      mask = diffcmp2mask_neon(range - 3, offset, pattern);
      if (mask) {
#if !defined(ENABLE_MEMCHECK) && !defined(__SANITIZE_ADDRESS__)
      found:
#endif /* !ENABLE_MEMCHECK && !__SANITIZE_ADDRESS__ */
        /* The sizeof(size_t) here is used correctly, since both the lane size, and the width and format of mask,
         * is also depend on the platform bitness. */
        return ptr_disp(range, -(clz_uintptr(mask) >> sizeof(size_t) / 4));
      }
      range -= 4;
    } while (range > detent + 3);
    if (range == detent)
      return nullptr;
  }

  /* Далее происходит чтение от 4 до 12 лишних байт, которые могут быть не
   * только за пределами региона выделенного под PNL, но и пересекать границу
   * страницы памяти. Что может приводить как к ошибкам ASAN, так и к падению.
   * Поэтому проверяем смещение на странице, а с ASAN всегда страхуемся. */
#if !defined(ENABLE_MEMCHECK) && !defined(__SANITIZE_ADDRESS__)
  const unsigned on_page_safe_mask = 0xff0 /* enough for '-15' bytes offset */;
  if (likely(on_page_safe_mask & (uintptr_t)(range + offset)) && !RUNNING_ON_VALGRIND) {
    const unsigned extra = (unsigned)(detent + 4 - range);
    ASSERT(extra > 0 && extra < 4);
    mask = (~(size_t)0) << (extra * sizeof(size_t) * 2);
    mask &= diffcmp2mask_neon(range - 3, offset, pattern);
    if (mask)
      goto found;
    return nullptr;
  }
#endif /* !ENABLE_MEMCHECK && !__SANITIZE_ADDRESS__ */
  do
    if (*range - range[offset] == target)
      return range;
  while (--range != detent);
  return nullptr;
}
#endif /* __ARM_NEON || __ARM_NEON__ */

#if defined(__AVX512BW__) && defined(MDBX_ATTRIBUTE_TARGET_AVX512BW)
#define scan4seq_default scan4seq_avx512bw
#define scan4seq_impl scan4seq_default
#elif defined(__AVX2__) && defined(MDBX_ATTRIBUTE_TARGET_AVX2)
#define scan4seq_default scan4seq_avx2
#elif defined(__SSE2__) && defined(MDBX_ATTRIBUTE_TARGET_SSE2)
#define scan4seq_default scan4seq_sse2
#elif (defined(__ARM_NEON) || defined(__ARM_NEON__)) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define scan4seq_default scan4seq_neon
/* Choosing of another variants should be added here. */
#endif /* scan4seq_default */

#endif /* MDBX_PNL_ASCENDING */

#ifndef scan4seq_default
#define scan4seq_default scan4seq_fallback
#endif /* scan4seq_default */

#ifdef scan4seq_impl
/* The scan4seq_impl() is the best or no alternatives */
#elif !MDBX_HAVE_BUILTIN_CPU_SUPPORTS
/* The scan4seq_default() will be used since no cpu-features detection support
 * from compiler. Please don't ask to implement cpuid-based detection and don't
 * make such PRs. */
#define scan4seq_impl scan4seq_default
#else
/* Selecting the most appropriate implementation at runtime,
 * depending on the available CPU features. */
static pgno_t *scan4seq_resolver(pgno_t *range, const size_t len, const size_t seq);
static pgno_t *(*scan4seq_impl)(pgno_t *range, const size_t len, const size_t seq) = scan4seq_resolver;
static pgno_t *scan4seq_resolver(pgno_t *range, const size_t len, const size_t seq) {
  pgno_t *(*choice)(pgno_t *range, const size_t len, const size_t seq) = nullptr;
#if __has_builtin(__builtin_cpu_init) || defined(__BUILTIN_CPU_INIT__) || __GNUC_PREREQ(4, 8)
  __builtin_cpu_init();
#endif /* __builtin_cpu_init() */
#ifdef MDBX_ATTRIBUTE_TARGET_SSE2
  if (__builtin_cpu_supports("sse2"))
    choice = scan4seq_sse2;
#endif /* MDBX_ATTRIBUTE_TARGET_SSE2 */
#ifdef MDBX_ATTRIBUTE_TARGET_AVX2
  if (__builtin_cpu_supports("avx2"))
    choice = scan4seq_avx2;
#endif /* MDBX_ATTRIBUTE_TARGET_AVX2 */
#ifdef MDBX_ATTRIBUTE_TARGET_AVX512BW
  if (__builtin_cpu_supports("avx512bw"))
    choice = scan4seq_avx512bw;
#endif /* MDBX_ATTRIBUTE_TARGET_AVX512BW */
  /* Choosing of another variants should be added here. */
  scan4seq_impl = choice ? choice : scan4seq_default;
  return scan4seq_impl(range, len, seq);
}
#endif /* scan4seq_impl */
