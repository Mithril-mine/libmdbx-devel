/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint32_unchecked(const size_t expected_alignment,
                                                                                const MDBX_val *a, const MDBX_val *b) {
  ASSERT(a->iov_len == 4 && b->iov_len == 4);
  return CMP2INT(unaligned_peek_u32(expected_alignment, a->iov_base),
                 unaligned_peek_u32(expected_alignment, b->iov_base));
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t
cmp_uint32_unaligned_unchecked(const MDBX_val *a, const MDBX_val *b) {
  return cmp_uint32_unchecked(1, a, b);
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint64_unchecked(const size_t expected_alignment,
                                                                                const MDBX_val *a, const MDBX_val *b) {
  ASSERT(a->iov_len == 8 && b->iov_len == 8);
  return CMP2INT(unaligned_peek_u64(expected_alignment, a->iov_base),
                 unaligned_peek_u64(expected_alignment, b->iov_base));
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t
cmp_uint64_unaligned_unchecked(const MDBX_val *a, const MDBX_val *b) {
  return cmp_uint64_unchecked(1, a, b);
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint(const size_t expected_alignment, const MDBX_val *a,
                                                                    const MDBX_val *b) {
  if (likely(a->iov_len == b->iov_len)) {
    if (sizeof(size_t) > 7 && likely(a->iov_len == 8))
      return cmp_uint64_unchecked(expected_alignment, a, b);
    if (likely(a->iov_len == 4))
      return cmp_uint32_unchecked(expected_alignment, a, b);
    if (sizeof(size_t) < 8 && likely(a->iov_len == 8))
      return cmp_uint64_unchecked(expected_alignment, a, b);
  }
  ERROR("mismatch and/or invalid size %p.%zu/%p.%zu for INTEGERKEY/INTEGERDUP", a->iov_base, a->iov_len, b->iov_base,
        b->iov_len);
  return 0;
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint32(const size_t expected_alignment,
                                                                      const MDBX_val *a, const MDBX_val *b) {
  if (likely(a->iov_len == b->iov_len && a->iov_len == 4))
    return cmp_uint32_unchecked(expected_alignment, a, b);
  ERROR("mismatch and/or invalid size %p.%zu/%p.%zu for INTEGERKEY/INTEGERDUP", a->iov_base, a->iov_len, b->iov_base,
        b->iov_len);
  return 0;
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint64(const size_t expected_alignment,
                                                                      const MDBX_val *a, const MDBX_val *b) {
  if (likely(a->iov_len == b->iov_len && a->iov_len == 8))
    return cmp_uint64_unchecked(expected_alignment, a, b);
  ERROR("mismatch and/or invalid size %p.%zu/%p.%zu for INTEGERKEY/INTEGERDUP", a->iov_base, a->iov_len, b->iov_base,
        b->iov_len);
  return 0;
}

#if MDBX_UNALIGNED_OK < 2 || MDBX_CHECKING > 0
/* Compare two items pointing at 2-byte aligned unsigned int's. */
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint_align2(const MDBX_val *a,
                                                                                             const MDBX_val *b) {
  return cmp_uint(2, a, b);
}
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint32_align2(const MDBX_val *a,
                                                                                               const MDBX_val *b) {
  return cmp_uint32(2, a, b);
}
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint64_align2(const MDBX_val *a,
                                                                                               const MDBX_val *b) {
  return cmp_uint64(2, a, b);
}
/* Currently unused
  MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint32_align2_unchecked(const
  MDBX_val *a, const MDBX_val *b) { return cmp_uint32_unchecked(2, a, b);
  }
  MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint64_align2_unchecked(const
  MDBX_val *a, const MDBX_val *b) { return cmp_uint64_unchecked(2, a, b);
  } */
#else
#define cmp_uint_align2 cmp_uint_unaligned
#define ncmp_uint_align2 ncmp_uint_unaligned
/* Currently unused
#define cmp_uint32_align2 cmp_uint32_unaligned
#define cmp_uint64_align2 cmp_uint64_unaligned
#define cmp_uint32_align2_unchecked cmp_uint32_unaligned_unchecked
#define cmp_uint64_align2_unchecked cmp_uint64_unaligned_unchecked */
#endif /* !MDBX_UNALIGNED_OK || debug */

#if MDBX_UNALIGNED_OK < 4 || MDBX_CHECKING > 0
/* Compare two items pointing at 4-byte aligned unsigned int's. */
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint_align4(const MDBX_val *a,
                                                                                             const MDBX_val *b) {
  return cmp_uint(4, a, b);
}
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint32_align4(const MDBX_val *a,
                                                                                               const MDBX_val *b) {
  return cmp_uint32(4, a, b);
}
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint64_align4(const MDBX_val *a,
                                                                                               const MDBX_val *b) {
  return cmp_uint64(4, a, b);
}
/* Currently unused
  MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL intptr_t cmp_uint32_align4(const MDBX_val *a, const MDBX_val *b);
  MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL intptr_t cmp_uint64_align4(const MDBX_val *a, const MDBX_val *b); */
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t
cmp_uint32_align4_unchecked(const MDBX_val *a, const MDBX_val *b) {
  return cmp_uint32_unchecked(4, a, b);
}
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t
cmp_uint64_align4_unchecked(const MDBX_val *a, const MDBX_val *b) {
  return cmp_uint64_unchecked(4, a, b);
}
#else
#define cmp_uint_align4 cmp_uint_unaligned
#define ncmp_uint_align4 ncmp_uint_unaligned
/* Currently unused
  #define cmp_uint32_align4 cmp_uint32_unaligned
  #define cmp_uint64_align4 cmp_uint64_unaligned */
#define cmp_uint32_align4_unchecked cmp_uint32_unaligned_unchecked
#define cmp_uint64_align4_unchecked cmp_uint64_unaligned_unchecked
#endif /* !MDBX_UNALIGNED_OK || debug */

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint_unaligned(const MDBX_val *a,
                                                                                                const MDBX_val *b) {
  return cmp_uint(1, a, b);
}
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint32_unaligned(const MDBX_val *a,
                                                                                                  const MDBX_val *b) {
  return cmp_uint32(1, a, b);
}
MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_uint64_unaligned(const MDBX_val *a,
                                                                                                  const MDBX_val *b) {
  return cmp_uint64(1, a, b);
}

/*----------------------------------------------------------------------------*/

MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL bool eq_fast_slowpath(const uint8_t *a, const uint8_t *b, size_t l);

MDBX_NOTHROW_PURE_FUNCTION static inline bool eq_fast(const MDBX_val *a, const MDBX_val *b) {
  return a->iov_len == b->iov_len && eq_fast_slowpath(a->iov_base, b->iov_base, a->iov_len);
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL int ncmp_equal_or_greater(const MDBX_val *a, const MDBX_val *b);

MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL int ncmp_equal_or_wrong(const MDBX_val *a, const MDBX_val *b);

MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_len(size_t a, size_t b) {
  const intptr_t diff_len = a - b;
  ASSERT(diff_len == (int)diff_len);
  /* кастинг допустим, так как длина ключей проверяется и не должна превышать INT_MAX / 2. */
  return diff_len;
}

/* Compare two items lexically */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_lexical(const MDBX_val *a, const MDBX_val *b) {
  const intptr_t diff_len = cmp_len(a->iov_len, b->iov_len);
  const size_t shortest = (a->iov_len < b->iov_len) ? a->iov_len : b->iov_len;
  int diff_data = likely(shortest) ? memcmp(a->iov_base, b->iov_base, shortest) : 0;
  return likely(diff_data) ? diff_data : diff_len;
}

MDBX_NOTHROW_PURE_FUNCTION static __always_inline unsigned tail3le(const uint8_t *p, size_t l) {
  STATIC_ASSERT(sizeof(unsigned) > 2);
  // 1: 0 0 0
  // 2: 0 1 1
  // 3: 0 1 2
  return p[0] | p[l >> 1] << 8 | p[l - 1] << 16;
}

/* Compare two items in reverse byte order */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_reverse(const MDBX_val *a, const MDBX_val *b) {
  size_t left = (a->iov_len < b->iov_len) ? a->iov_len : b->iov_len;
  if (likely(left)) {
    const uint8_t *pa = ptr_disp(a->iov_base, a->iov_len);
    const uint8_t *pb = ptr_disp(b->iov_base, b->iov_len);
    while (left >= sizeof(size_t)) {
      pa -= sizeof(size_t);
      pb -= sizeof(size_t);
      left -= sizeof(size_t);
      STATIC_ASSERT(sizeof(size_t) == 4 || sizeof(size_t) == 8);
      if (sizeof(size_t) == 4) {
        uint32_t xa = unaligned_peek_u32(1, pa);
        uint32_t xb = unaligned_peek_u32(1, pb);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
        xa = osal_bswap32(xa);
        xb = osal_bswap32(xb);
#endif /* __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__ */
        if (xa != xb)
          return (xa < xb) ? -1 : 1;
      } else {
        uint64_t xa = unaligned_peek_u64(1, pa);
        uint64_t xb = unaligned_peek_u64(1, pb);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
        xa = osal_bswap64(xa);
        xb = osal_bswap64(xb);
#endif /* __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__ */
        if (xa != xb)
          return (xa < xb) ? -1 : 1;
      }
    }
    if (sizeof(size_t) == 8 && left >= 4) {
      pa -= 4;
      pb -= 4;
      left -= 4;
      uint32_t xa = unaligned_peek_u32(1, pa);
      uint32_t xb = unaligned_peek_u32(1, pb);
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
      xa = osal_bswap32(xa);
      xb = osal_bswap32(xb);
#endif /* __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__ */
      if (xa != xb)
        return (xa < xb) ? -1 : 1;
    }
    if (left) {
      unsigned xa = tail3le(pa - left, left);
      unsigned xb = tail3le(pb - left, left);
      if (xa != xb)
        return (xa < xb) ? -1 : 1;
    }
  }
  return cmp_len(a->iov_len, b->iov_len);
}

/* Fast non-lexically comparator */
MDBX_NOTHROW_PURE_FUNCTION static __always_inline intptr_t cmp_lenfast(const MDBX_val *a, const MDBX_val *b) {
  int diff = cmp_len(a->iov_len, b->iov_len);
  return (likely(diff) || a->iov_len == 0) ? diff : memcmp(a->iov_base, b->iov_base, a->iov_len);
}

/*----------------------------------------------------------------------------*/

#ifndef ncmp_uint_align2
MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL int ncmp_uint_align2(const MDBX_val *a, const MDBX_val *b);
#endif
#ifndef ncmp_uint_align4
MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL int ncmp_uint_align4(const MDBX_val *a, const MDBX_val *b);
#endif
MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL int ncmp_uint_unaligned(const MDBX_val *a, const MDBX_val *b);
MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL int ncmp_lexical(const MDBX_val *a, const MDBX_val *b);
MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL int ncmp_reverse(const MDBX_val *a, const MDBX_val *b);

static inline MDBX_cmp_func builtin_keycmp(MDBX_db_flags_t flags) {
  return (flags & MDBX_INTEGERKEY) ? ncmp_uint_align2 : !(flags & MDBX_REVERSEKEY) ? ncmp_lexical : ncmp_reverse;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL int ncmp_lenfast(const MDBX_val *a, const MDBX_val *b);

static inline MDBX_cmp_func builtin_datacmp(MDBX_db_flags_t flags) {
  if (flags & MDBX_DUPSORT) {
    if (flags & MDBX_INTEGERDUP)
      return (flags & MDBX_INTEGERKEY) ? /* aligned since keys length are aligned */ ncmp_uint_align4
                                       : /* may be unaligned since key length may vary */ ncmp_uint_unaligned;
    return (flags & MDBX_REVERSEDUP) ? ncmp_reverse : ncmp_lexical;
  }
  return ncmp_lenfast;
}

MDBX_INTERNAL size_t tree_search_branch_configure(const MDBX_cursor *mc, const MDBX_val *key);
MDBX_INTERNAL sfr_t tree_search_foliage_configure(MDBX_cursor *mc, const MDBX_val *key);

static inline void clc_reset_methods(volatile clc_t *clc) {
  clc->search_branch = tree_search_branch_configure;
  clc->search_foliage = tree_search_foliage_configure;
}
