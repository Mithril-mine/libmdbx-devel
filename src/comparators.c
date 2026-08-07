/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

int ncmp_equal_or_greater(const MDBX_val *a, const MDBX_val *b) { return eq_fast(a, b) ? 0 : 1; }

int ncmp_equal_or_wrong(const MDBX_val *a, const MDBX_val *b) { return eq_fast(a, b) ? 0 : -1; }

__hot bool eq_fast_slowpath(const uint8_t *a, const uint8_t *b, size_t l) {
  if (likely(l > 3)) {
    if (MDBX_UNALIGNED_OK >= 4 && likely(l < 9))
      return ((unaligned_peek_u32(1, a) - unaligned_peek_u32(1, b)) |
              (unaligned_peek_u32(1, a + l - 4) - unaligned_peek_u32(1, b + l - 4))) == 0;
    if (MDBX_UNALIGNED_OK >= 8 && sizeof(size_t) > 7 && likely(l < 17))
      return ((unaligned_peek_u64(1, a) - unaligned_peek_u64(1, b)) |
              (unaligned_peek_u64(1, a + l - 8) - unaligned_peek_u64(1, b + l - 8))) == 0;
    return memcmp(a, b, l) == 0;
  }
  if (likely(l))
    return tail3le(a, l) == tail3le(b, l);
  return true;
}

/*----------------------------------------------------------------------------*/

#define NONINLINE_OLDAPI_COMPARATOR(name)                                                                              \
  __hot int n##name(const MDBX_val *a, const MDBX_val *b) {                                                            \
    intptr_t diff = name(a, b);                                                                                        \
    ASSERT(diff == (int)diff);                                                                                         \
    return (int)diff;                                                                                                  \
  }

NONINLINE_OLDAPI_COMPARATOR(cmp_lexical)
NONINLINE_OLDAPI_COMPARATOR(cmp_reverse)
NONINLINE_OLDAPI_COMPARATOR(cmp_lenfast)
NONINLINE_OLDAPI_COMPARATOR(cmp_uint_unaligned)
#ifndef cmp_uint_align2
NONINLINE_OLDAPI_COMPARATOR(cmp_uint_align2)
#endif
#ifndef cmp_uint_align4
NONINLINE_OLDAPI_COMPARATOR(cmp_uint_align4)
#endif
