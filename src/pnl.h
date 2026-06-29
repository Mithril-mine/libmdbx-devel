/// \copyright SPDX-License-Identifier: Apache-2.0
/// \note Please refer to the COPYRIGHT file for explanation of license change,
/// credits and acknowledgments.
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

/* An PNL is an Page Number List, a sorted array of IDs.
 *
 * The first element of the array is a counter for how many actual page-numbers
 * are in the list. By default PNLs are sorted in descending order, this allow
 * cut off a page with lowest pgno (at the tail) just truncating the list. The
 * sort order of PNLs is controlled by the MDBX_PNL_ASCENDING build option. */
typedef pgno_t *pnl_t;
typedef const pgno_t *const_pnl_t;

#define MDBX_PNL_FIRST(pl) ((pl)[1])
#define MDBX_PNL_LAST(pl) ((pl)[pnl_size(pl)])
#define MDBX_PNL_BEGIN(pl) (&(pl)[1])
#define MDBX_PNL_END(pl) (&(pl)[pnl_size(pl) + 1])

#if MDBX_PNL_ASCENDING
#define MDBX_PNL_ORDERED(first, last) ((first) < (last))
#define MDBX_PNL_DISORDERED(first, last) ((first) >= (last))
#define MDBX_PNL_REVERSED(first, last) ((first) > (last))
#define MDBX_PNL_EDGE(pl) ((pl) + 1)
#define MDBX_PNL_LEAST(pl) MDBX_PNL_FIRST(pl)
#define MDBX_PNL_MOST(pl) MDBX_PNL_LAST(pl)
#define MDBX_PNL_CONTIGUOUS(prev, next, span) (((next) - (prev)) == (span))
#else
#define MDBX_PNL_ORDERED(first, last) ((first) > (last))
#define MDBX_PNL_DISORDERED(first, last) ((first) <= (last))
#define MDBX_PNL_REVERSED(first, last) ((first) < (last))
#define MDBX_PNL_EDGE(pl) ((pl) + pnl_size(pl))
#define MDBX_PNL_LEAST(pl) MDBX_PNL_LAST(pl)
#define MDBX_PNL_MOST(pl) MDBX_PNL_FIRST(pl)
#define MDBX_PNL_CONTIGUOUS(prev, next, span) (((prev) - (next)) == (span))
#endif /* MDBX_PNL_ASCENDING */

#ifndef __cplusplus

#define MDBX_PNL_GRANULATE_LOG2 10
#define MDBX_PNL_GRANULATE (1 << MDBX_PNL_GRANULATE_LOG2)
#define MDBX_PNL_INITIAL (MDBX_PNL_GRANULATE - 2 - MDBX_ASSUME_MALLOC_OVERHEAD / sizeof(pgno_t))

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline size_t pnl_alloclen(const const_pnl_t pnl) {
  return pnl[-1];
}

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline size_t pnl_size(const const_pnl_t pnl) { return pnl[0]; }

MDBX_MAYBE_UNUSED static inline void pnl_setsize(pnl_t pnl, size_t len) {
  ASSERT(len < INT_MAX);
  pnl[0] = (pgno_t)len;
}

MDBX_MAYBE_UNUSED static inline void pnl_clear(pnl_t pnl) { pnl_setsize(pnl, 0); }

#define MDBX_PNL_SIZEOF(pl) ((pnl_size(pl) + 1) * sizeof(pgno_t))
#define MDBX_PNL_IS_EMPTY(pl) (pnl_size(pl) == 0)

MDBX_NOTHROW_PURE_FUNCTION MDBX_MAYBE_UNUSED static inline pgno_t pnl_bytes2size(const size_t bytes) {
  size_t size = bytes / sizeof(pgno_t);
  ASSERT(size > 3 && size <= PAGELIST_LIMIT + /* alignment gap */ 65536);
  size -= 3;
#if MDBX_PNL_PREALLOC_FOR_RADIXSORT
  size >>= 1;
#endif /* MDBX_PNL_PREALLOC_FOR_RADIXSORT */
  return (pgno_t)size;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_MAYBE_UNUSED static inline size_t pnl_size2bytes(size_t wanna_size) {
  size_t size = wanna_size;
  ASSERT(size > 0 && size <= PAGELIST_LIMIT);
#if MDBX_PNL_PREALLOC_FOR_RADIXSORT
  size += size;
#endif /* MDBX_PNL_PREALLOC_FOR_RADIXSORT */
  STATIC_ASSERT(MDBX_ASSUME_MALLOC_OVERHEAD +
                    (PAGELIST_LIMIT * (MDBX_PNL_PREALLOC_FOR_RADIXSORT + 1) + MDBX_PNL_GRANULATE + 3) * sizeof(pgno_t) <
                SIZE_MAX / 4 * 3);
  size_t bytes =
      ceil_powerof2(MDBX_ASSUME_MALLOC_OVERHEAD + sizeof(pgno_t) * (size + 3), MDBX_PNL_GRANULATE * sizeof(pgno_t)) -
      MDBX_ASSUME_MALLOC_OVERHEAD;
  ASSERT(pnl_bytes2size(bytes) >= wanna_size);
  return bytes;
}

MDBX_INTERNAL pnl_t pnl_alloc(size_t size);

MDBX_INTERNAL void pnl_free(pnl_t pnl);

MDBX_MAYBE_UNUSED MDBX_INTERNAL pnl_t pnl_clone(const const_pnl_t src);

MDBX_INTERNAL int pnl_reserve(pnl_t __restrict *__restrict ppnl, const size_t wanna);

MDBX_MAYBE_UNUSED static inline int __must_check_result pnl_need(pnl_t __restrict *__restrict ppnl, size_t more) {
  if (likely(*ppnl)) {
    ASSERT(pnl_size(*ppnl) <= PAGELIST_LIMIT && pnl_alloclen(*ppnl) >= pnl_size(*ppnl));
    ASSERT(more <= PAGELIST_LIMIT);
    more += pnl_size(*ppnl);
    if (likely(pnl_alloclen(*ppnl) >= more))
      return MDBX_SUCCESS;
  }
  return pnl_reserve(ppnl, more);
}

MDBX_MAYBE_UNUSED static inline void pnl_append_prereserved(__restrict pnl_t pnl, pgno_t pgno) {
  ASSERT(pnl_size(pnl) < pnl_alloclen(pnl));
  if (CHECKS2_ENABLED()) {
    for (size_t i = pnl_size(pnl); i > 0; --i)
      ASSERT(pgno != pnl[i]);
  }
  *pnl += 1;
  MDBX_PNL_LAST(pnl) = pgno;
}

MDBX_MAYBE_UNUSED static inline int __must_check_result pnl_append(__restrict pnl_t *ppnl, pgno_t pgno) {
  int rc = pnl_need(ppnl, 1);
  if (likely(rc == MDBX_SUCCESS))
    pnl_append_prereserved(*ppnl, pgno);
  return rc;
}

MDBX_INTERNAL void pnl_shrink(pnl_t __restrict *__restrict ppnl);

MDBX_INTERNAL int __must_check_result spill_append_span(__restrict pnl_t *ppnl, pgno_t pgno, size_t n);

MDBX_INTERNAL int __must_check_result pnl_append_span(__restrict pnl_t *ppnl, pgno_t pgno, size_t n);

MDBX_INTERNAL int __must_check_result pnl_append_pnl(__restrict pnl_t *ppnl, const const_pnl_t src);

MDBX_INTERNAL int __must_check_result pnl_insert_span(__restrict pnl_t *ppnl, pgno_t pgno, size_t n);

MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL size_t pnl_search_nochk(const const_pnl_t pnl, pgno_t pgno);

MDBX_INTERNAL void pnl_sort_nochk(pnl_t pnl);

MDBX_INTERNAL bool pnl_check(const const_pnl_t pnl, const size_t limit);

MDBX_MAYBE_UNUSED static inline bool pnl_check_allocated(const const_pnl_t pnl, const size_t limit) {
  return pnl == nullptr || (pnl_alloclen(pnl) >= pnl_size(pnl) && pnl_check(pnl, limit));
}

MDBX_MAYBE_UNUSED static inline void pnl_sort(pnl_t pnl, size_t limit4check) {
  pnl_sort_nochk(pnl);
  ASSERT(pnl_check(pnl, limit4check));
  (void)limit4check;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_MAYBE_UNUSED static inline size_t pnl_search(const const_pnl_t pnl, pgno_t pgno,
                                                                             size_t limit) {
  ASSERT(pnl_check_allocated(pnl, limit));
  if (MDBX_HAVE_CMOV) {
    /* cmov-ускоренный бинарный поиск может читать (но не использовать) один
     * элемент за концом данных, этот элемент в пределах выделенного участка
     * памяти, но не инициализирован. */
    VALGRIND_MAKE_MEM_DEFINED(MDBX_PNL_END(pnl), sizeof(pgno_t));
  }
  ASSERT(pgno < limit);
  (void)limit;
  size_t n = pnl_search_nochk(pnl, pgno);
  if (MDBX_HAVE_CMOV) {
    VALGRIND_MAKE_MEM_UNDEFINED(MDBX_PNL_END(pnl), sizeof(pgno_t));
  }
  return n;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_MAYBE_UNUSED static inline size_t pnl_search_exact(const const_pnl_t pnl, pgno_t pgno) {
  size_t n = pnl_search_nochk(pnl, pgno);
  return (n <= pnl_size(pnl) && pnl[n] == pgno) ? n : 0;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_MAYBE_UNUSED static inline bool pnl_contains(const const_pnl_t pnl, pgno_t pgno) {
  size_t n = pnl_search_nochk(pnl, pgno);
  return n <= pnl_size(pnl) && pnl[n] == pgno;
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_MAYBE_UNUSED static inline bool pnl_contains_span(const const_pnl_t pnl, pgno_t pgno,
                                                                                  pgno_t span) {
  size_t n = pnl_search_nochk(pnl, pgno);
#if MDBX_PNL_ASCENDING
#error "FIXME: Since 2026-04-01 alternatives to MDBX_PNL_ASCENDING = 0 are no longer supported."
#else
  return n >= span && pnl[n] == pgno && MDBX_PNL_CONTIGUOUS(pnl[n - span + 1], pnl[n], span - 1);
#endif /* MDBX_PNL_ASCENDING */
}

MDBX_INTERNAL size_t pnl_merge(pnl_t dst, const const_pnl_t src);

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL size_t pnl_maxspan(const const_pnl_t pnl);

MDBX_MAYBE_UNUSED MDBX_NOTHROW_PURE_FUNCTION static inline size_t pnl_scan_span(const const_pnl_t pnl,
                                                                                const size_t from) {
  size_t span = 1;
  ASSERT(from > 0 && from <= pnl_size(pnl));
  while (from + span <= pnl_size(pnl) && MDBX_PNL_CONTIGUOUS(pnl[from], pnl[from + span], span))
    ++span;
  return span;
}

MDBX_MAYBE_UNUSED MDBX_INTERNAL pgno_t pnl_get_best_sequence(const pnl_t pnl, const size_t span,
                                                             const pgno_t defrag_detent);
MDBX_MAYBE_UNUSED MDBX_INTERNAL pgno_t pnl_crop_tail_sequence(const pnl_t pnl);

MDBX_MAYBE_UNUSED MDBX_INTERNAL void pnl_cut(pnl_t pnl, size_t pos, size_t len);

MDBX_MAYBE_UNUSED MDBX_INTERNAL void pnl_sift(pnl_t pnl, const const_pnl_t filter_out);

MDBX_MAYBE_UNUSED MDBX_INTERNAL int pnl_cut_range(__restrict pnl_t pnl, __restrict pnl_t *const pdest,
                                                  pgno_t range_begin, pgno_t range_end);

#endif /* !__cplusplus */
