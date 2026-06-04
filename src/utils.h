/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

/* Test if the flags f are set in a flag word w. */
#define F_ISSET(w, f) (((w) & (f)) == (f))

/* Round n up to an even number. */
#define EVEN_CEIL(n) (((n) + 1UL) & -2L) /* sign-extending -2 to match n+1U */

/* Round n down to an even number. */
#define EVEN_FLOOR(n) ((n) & ~(size_t)1)

/*
 *                /
 *                | -1, a < b
 * CMP2INT(a,b) = <  0, a == b
 *                |  1, a > b
 *                \
 */
#define CMP2INT(a, b) (((a) != (b)) ? (((a) < (b)) ? -1 : 1) : 0)

/* Pointer displacement without casting to char* to avoid pointer-aliasing */
#define ptr_disp(ptr, disp) ((void *)(((intptr_t)(ptr)) + ((intptr_t)(disp))))

/* Pointer distance as signed number of bytes */
#define ptr_dist(more, less) (((intptr_t)(more)) - ((intptr_t)(less)))

#define MDBX_ASAN_POISON_MEMORY_REGION(addr, size)                                                                     \
  do {                                                                                                                 \
    TRACE("POISON_MEMORY_REGION(%p, %zu) at %u", (void *)(addr), (size_t)(size), __LINE__);                            \
    ASAN_POISON_MEMORY_REGION(addr, size);                                                                             \
  } while (0)

#define MDBX_ASAN_UNPOISON_MEMORY_REGION(addr, size)                                                                   \
  do {                                                                                                                 \
    TRACE("UNPOISON_MEMORY_REGION(%p, %zu) at %u", (void *)(addr), (size_t)(size), __LINE__);                          \
    ASAN_UNPOISON_MEMORY_REGION(addr, size);                                                                           \
  } while (0)

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED static inline size_t branchless_abs(intptr_t value) {
  ASSERT(value > INT_MIN);
  const size_t expanded_sign = (size_t)(value >> (sizeof(value) * CHAR_BIT - 1));
  return ((size_t)value + expanded_sign) ^ expanded_sign;
}

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED static inline intptr_t max_signed(intptr_t a, intptr_t b) {
  return (a > b) ? a : b;
}

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED static inline intptr_t min_signed(intptr_t a, intptr_t b) {
  return (a < b) ? a : b;
}

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED static inline intptr_t clamp_signed(intptr_t v, intptr_t min,
                                                                                  intptr_t max) {
  ASSERT(min <= max);
  return min_signed(max_signed(v, min), max);
}

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED static inline size_t max_unsigned(size_t a, size_t b) {
  return (a > b) ? a : b;
}

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED static inline size_t min_unsigned(size_t a, size_t b) {
  return (a < b) ? a : b;
}

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED static inline size_t clamp_unsigned(size_t v, size_t min, size_t max) {
  ASSERT(min <= max);
  return min_unsigned(max_unsigned(v, min), max);
}

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED static inline bool is_powerof2(size_t x) { return (x & (x - 1)) == 0; }

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED static inline size_t floor_powerof2(size_t value, size_t granularity) {
  ASSERT(is_powerof2(granularity));
  return value & ~(granularity - 1);
}

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED static inline size_t ceil_powerof2(size_t value, size_t granularity) {
  return floor_powerof2(value + granularity - 1, granularity);
}

#ifndef __cplusplus

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED MDBX_INTERNAL unsigned log2n_powerof2(size_t value_uintptr);

MDBX_NOTHROW_CONST_FUNCTION MDBX_MAYBE_UNUSED MDBX_INTERNAL unsigned ceil_log2n(size_t value_uintptr);

MDBX_NOTHROW_CONST_FUNCTION MDBX_INTERNAL uint64_t rrxmrrxmsx_0(uint64_t v);

struct monotime_cache {
  uint64_t value;
  int expire_countdown;
};

MDBX_MAYBE_UNUSED static inline uint64_t monotime_since_cached(uint64_t begin_timestamp, struct monotime_cache *cache) {
  if (cache->expire_countdown)
    cache->expire_countdown -= 1;
  else {
    cache->value = osal_monotime();
    cache->expire_countdown = 42 / 3;
  }
  return cache->value - begin_timestamp;
}

typedef struct ratio2digits_buffer {
  char string[1 + 20 + 1 + 19 + 1];
} ratio2digits_buffer_t;

MDBX_INTERNAL char *ratio2digits(const uint64_t v, const uint64_t d, ratio2digits_buffer_t *const buffer,
                                 int precision);
MDBX_INTERNAL char *ratio2percent(uint64_t v, uint64_t d, ratio2digits_buffer_t *buffer);

MDBX_INTERNAL bin128_t mul64x64_128_fallback(uint64_t x, uint64_t y);

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IA64) || defined(_M_AMD64))
#pragma intrinsic(_umul128)
#endif /* MSVC AMD64 */

#if defined(_MSC_VER) && defined(_M_ARM64)
#pragma intrinsic(__umulh)
#endif /* MSVC ARM64 */

MDBX_MAYBE_UNUSED static inline bool u128_eq(bin128_t x, bin128_t y) {
#if defined(__SIZEOF_INT128__) && MDBX_CHECKING < 1
  return x.u128 == y.u128;
#else
  return x.l == y.l && x.h == y.h;
#endif
}

MDBX_MAYBE_UNUSED static inline bin128_t mul64x64_128(uint64_t x, uint64_t y) {
  bin128_t r;
#if MDBX_HAVE_NATIVE_U128 && MDBX_CHECKING < 1
  r.u128 = x;
  r.u128 *= y;
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IA64) || defined(_M_AMD64)) && MDBX_CHECKING < 1
  r.l = _umul128(x, y, &r.h);
#elif defined(_MSC_VER) && defined(_M_ARM64) && MDBX_CHECKING < 1
  r.l = x * y;
  r.h = __umulh(x, y);
#else
  r = mul64x64_128_fallback(x, y);
#endif
  ASSERT(u128_eq(r, mul64x64_128_fallback(y, x)));
  return r;
}

MDBX_MAYBE_UNUSED static inline bin128_t u128_add(bin128_t x, bin128_t y) {
  bin128_t r;
#if MDBX_HAVE_NATIVE_U128 && MDBX_CHECKING < 1
  r.u128 = x.u128 + y.u128;
#else
  r.l = x.l + y.l;
  r.h = x.h + y.h + /* carry */ (r.l < x.l);
#if MDBX_HAVE_NATIVE_U128
  ASSERT(r.u128 == x.u128 + y.u128);
#endif
#endif
  return r;
}

MDBX_MAYBE_UNUSED static inline int u128_cmp(bin128_t x, bin128_t y) {
  int r;
#if defined(__SIZEOF_INT128__) && MDBX_CHECKING < 1
  r = CMP2INT(x.u128, y.u128);
#else
  const uint64_t a = (x.h != y.h) ? x.h : x.l;
  const uint64_t b = (x.h != y.h) ? y.h : y.l;
  r = CMP2INT(a, b);
#if MDBX_HAVE_NATIVE_U128
  ASSERT(r == CMP2INT(x.u128, y.u128));
#endif
#endif
  return r;
}

MDBX_MAYBE_UNUSED static inline bool u128_gt(bin128_t x, bin128_t y) {
  bool r;
#if defined(__SIZEOF_INT128__) && MDBX_CHECKING < 1
  r = x.u128 > y.u128;
#else
  r = x.h > y.h || (x.h == y.h && x.l > y.l);
#if MDBX_HAVE_NATIVE_U128
  ASSERT(r == (x.u128 > y.u128));
#endif
#endif
  return r;
}

MDBX_MAYBE_UNUSED static inline bool u128_lt(bin128_t x, bin128_t y) {
  bool r;
#if defined(__SIZEOF_INT128__) && MDBX_CHECKING < 1
  r = x.u128 < y.u128;
#else
  r = x.h < y.h || (x.h == y.h && x.l < y.l);
#if MDBX_HAVE_NATIVE_U128
  ASSERT(r == (x.u128 < y.u128));
#endif
#endif
  return r;
}

MDBX_MAYBE_UNUSED static inline bin128_t u128(uint64_t v) {
  bin128_t r;
  r.l = v;
  r.h = 0;
#if defined(__SIZEOF_INT128__)
  ASSERT(r.u128 == v);
#endif
  return r;
}

#endif /* !__cplusplus */
