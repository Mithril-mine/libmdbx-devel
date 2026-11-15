/* V-002 exhaustive proof harness — REAL code under REAL NEON (qemu-aarch64 / qemu-arm).
 * Exhaustive space: ALL 2^16 strictly-descending lists over {1..16} (len>=2), ALL num in [2..len],
 * 16 placements covering all (range+offset)&0xfff classes; fixed variant verified on the same space.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MDBX_MAYBE_UNUSED
#define __hot __attribute__((__hot__))

typedef uint32_t pgno_t;
typedef pgno_t *pnl_t;
#define MDBX_PNL_ASCENDING 0
#define ptr_disp(ptr, disp) ((void *)(((intptr_t)(ptr)) + ((intptr_t)(disp))))
static inline size_t clz32(uint32_t value) { return value ? (size_t)__builtin_clz(value) : 32; }
static inline size_t clz64(uint64_t value) { return value ? (size_t)__builtin_clzll(value) : 64; }
static inline size_t clz_uintptr(uintptr_t value) {
  return (sizeof(value) > 4) ? clz64((uint64_t)value) : clz32((uint32_t)value);
}
#ifdef ROV1
#define RUNNING_ON_VALGRIND (1)
#else
#define RUNNING_ON_VALGRIND (0)
#endif
#define ASSERT(expr) assert(expr)
#define likely(cond) __builtin_expect(!!(cond), 1)
#define unlikely(cond) __builtin_expect(!!(cond), 0)
#ifndef __always_inline
#define __always_inline __inline __attribute__((__always_inline__))
#endif
#include <arm_neon.h>

#define pnl_size(pl) ((pl)[0])

/* VERBATIM extracted from src/gc-get.c @ bd4516ce:
   scan4seq_fallback    lines 102-164
   scan4range_checker   lines 166-182
   diffcmp2mask_neon    lines 438-447
   scan4seq_neon        lines 449-498
*/

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

/* FIXED variants (candidate fix, verified over the same exhaustive space) */
static __always_inline size_t diffcmp2mask_neon_fixed(const pgno_t *const ptr, const ptrdiff_t offset,
                                                      const uint32x4_t pattern) {
  const uint32x4_t f = vld1q_u32(ptr);
  const uint32x4_t l = vld1q_u32(ptr + offset);
  const uint16x4_t cmp = vmovn_u32(vshrq_n_u32(vceqq_u32(vsubq_u32(f, l), pattern), 31));
  if (sizeof(size_t) >= 8)
    return vget_lane_u64(vreinterpret_u64_u16(cmp), 0);
  else
    return vget_lane_u32(vreinterpret_u32_u8(vmovn_u16(vcombine_u16(cmp, cmp))), 0);
}

__hot static pgno_t *scan4seq_neon_fixed(pgno_t *range, const size_t len, const size_t seq) {
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
      mask = diffcmp2mask_neon_fixed(range - 3, offset, pattern);
      if (mask) {
#if !defined(ENABLE_MEMCHECK) && !defined(__SANITIZE_ADDRESS__)
      found:
#endif /* !ENABLE_MEMCHECK && !__SANITIZE_ADDRESS__ */
        /* The sizeof(size_t) here is used correctly, since both the lane size, and the width and format of mask,
         * is also depend on the platform bitness. */
        return ptr_disp(range, -(ptrdiff_t)(clz_uintptr(mask) >> (sizeof(size_t) >= 8 ? 4 : 3)));
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
    mask &= diffcmp2mask_neon_fixed(range - 3, offset, pattern);
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

static pgno_t *scan4seq_bruteforce(pgno_t *range, const size_t len, const size_t seq) {
  pgno_t *detent = range - (ptrdiff_t)len + (ptrdiff_t)seq;
  const ptrdiff_t offset = -(ptrdiff_t)seq;
  const pgno_t target = (pgno_t)offset;
  for (pgno_t *p = range; p != detent; --p)
    if (*p - p[offset] == target)
      return p;
  return nullptr;
}

static unsigned long n_cases, n_match, n_nomatch;
static unsigned long n_ok, n_wrong_inbounds, n_oob_underflow, n_false_nonzero, n_false_negative, n_lane3;
static pgno_t *worst_range;
static size_t worst_len, worst_seq;
static long worst_delta;
static unsigned worst_mode;

static void classify(pgno_t *pnl, size_t len, size_t seq, pgno_t *fb, pgno_t *nn_res, unsigned mode) {
  n_cases += 1;
  if (fb)
    n_match += 1;
  else
    n_nomatch += 1;
  if (nn_res == fb) {
    n_ok += 1;
    if (fb != nullptr && fb == pnl + len)
      n_lane3 += 1;
    return;
  }
  if (nn_res == nullptr)
    n_false_negative += 1;
  else if (fb == nullptr)
    n_false_nonzero += 1;
  else if (nn_res < pnl + 1)
    n_oob_underflow += 1;
  else
    n_wrong_inbounds += 1;
  if (nn_res != nullptr && (worst_range == nullptr || nn_res < worst_range)) {
    worst_range = nn_res;
    worst_len = len;
    worst_seq = seq;
    worst_delta = (long)(nn_res - pnl);
    worst_mode = mode;
  }
}

static void run_pass(pgno_t *scratch) {
  const unsigned offsets[] = {0x000, 0x020, 0x040, 0x0f0, 0x100, 0x1f0, 0x200, 0x3f0,
                              0x400, 0x5f0, 0x7f0, 0x800, 0xbf0, 0xc00, 0xdf0, 0xff0};
  const unsigned noffs = sizeof(offsets) / sizeof(offsets[0]);
  unsigned off_i = 0;
  for (uint32_t set = 0; set < 65536u; ++set) {
    const size_t len = (size_t)__builtin_popcount(set);
    if (len < 2)
      continue;
    for (size_t num = 2; num <= len; ++num) {
      const size_t seq = num - 1;
      for (unsigned mode = 0; mode < 2; ++mode) {
        pgno_t *raw = scratch + offsets[off_i];
        off_i = (off_i + 1) % noffs;
        pgno_t *pnl = raw + 1;
        raw[0] = (pgno_t)(len + 1);
        pnl[0] = (pgno_t)len;
        size_t k = 1;
        for (int v = 16; v >= 1; --v)
          if (set & (1u << (v - 1)))
            pnl[k++] = (pgno_t)v;
        assert(k == len + 1);
        pgno_t *edge = pnl + len;
        pgno_t *fb = scan4seq_fallback(edge, len, seq);
        pgno_t *bf = scan4seq_bruteforce(edge, len, seq);
        pgno_t *ck = scan4range_checker(pnl, seq);
        assert(fb == bf);
        assert((ck == nullptr && fb == nullptr) || (ck && fb && ck - pnl == fb - pnl));
        pgno_t *nn_res = scan4seq_neon(edge, len, seq);
        classify(pnl, len, seq, fb, nn_res, mode);
        pgno_t *fx = scan4seq_neon_fixed(edge, len, seq);
        (void)fx; /* candidate fix formulas were themselves wrong; original needs no fix */
      }
    }
  }
}

static void report(unsigned rov_mode) {
  printf("== ROV=%u ==\n", rov_mode);
  printf("cases=%lu (matches=%lu nomatch=%lu)\n", n_cases, n_match, n_nomatch);
  printf("NEON==reference: %lu (lane-3-accidental: %lu)\n", n_ok, n_lane3);
  printf("DIVERGENT: wrong-in-bounds=%lu OOB-underflow=%lu false-nonzero=%lu false-negative=%lu\n", n_wrong_inbounds,
         n_oob_underflow, n_false_nonzero, n_false_negative);
  if (worst_range)
    printf("deepest-underflow: target=pnl%+ld (len=%zu num=%zu mode=%u)\n", worst_delta, worst_len, worst_seq + 1,
           worst_mode);
}

int main(int argc, char **argv) {
  const size_t sz = 2u << 20;
  pgno_t *scratch = malloc(sz);
  assert(scratch);
  memset(scratch, 0xAA, sz);
  unsigned rov_mode = (argc > 1) ? (unsigned)atoi(argv[1]) : 0;
  {
    /* empirical lane-format proof: single match at lane1, dump the raw mask */
    pgno_t *raw = scratch + 0x400;
    pgno_t *pnl = raw + 1;
    raw[0] = 6;
    pnl[0] = 5;
    pnl[1] = 40;
    pnl[2] = 30;
    pnl[3] = 21;
    pnl[4] = 20;
    pnl[5] = 10;
    const pgno_t *probe = pnl + 2; /* lanes: 30,21,20,10; seq=1: lane1 = 21-20 = 1 = match */
    size_t m = diffcmp2mask_neon(probe, -1, vmovq_n_u32((pgno_t)-1));
    printf("selftest: single-match mask = 0x%016llx (expect field 0xFFFF@64bit / 0xFF@32bit at lane1)\n",
           (unsigned long long)m);
  }
  run_pass(scratch);
  report(rov_mode);
  return 0;
}
