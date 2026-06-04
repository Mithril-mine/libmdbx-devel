/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#include "internals.h"

static inline size_t dml_size2bytes(ptrdiff_t size) {
  ASSERT(size >= CURSOR_STACK_SIZE && (size_t)size <= PAGELIST_LIMIT);
#if MDBX_DML_PREALLOC_FOR_RADIXSORT
  size += size;
#endif /* MDBX_DML_PREALLOC_FOR_RADIXSORT */
  STATIC_ASSERT(MDBX_ASSUME_MALLOC_OVERHEAD + sizeof(dml_t) +
                    (PAGELIST_LIMIT * (MDBX_DML_PREALLOC_FOR_RADIXSORT + 1)) * sizeof(da_t) +
                    MDBX_PNL_GRANULATE * sizeof(void *) * 2 <
                SIZE_MAX / 4 * 3);
  size_t bytes = ceil_powerof2(MDBX_ASSUME_MALLOC_OVERHEAD + sizeof(dml_t) + size * sizeof(da_t),
                               MDBX_PNL_GRANULATE * sizeof(void *) * 2) -
                 MDBX_ASSUME_MALLOC_OVERHEAD;
  return bytes;
}

static inline size_t dml_bytes2size(const ptrdiff_t bytes) {
  size_t size = (bytes - sizeof(dml_t)) / sizeof(da_t);
#if MDBX_DML_PREALLOC_FOR_RADIXSORT
  size >>= 1;
#endif /* MDBX_DML_PREALLOC_FOR_RADIXSORT */
  ASSERT(size > CURSOR_STACK_SIZE && size <= PAGELIST_LIMIT + MDBX_PNL_GRANULATE);
  return size;
}

__cold dml_t *dml_alloc(size_t size) {
  size_t bytes = dml_size2bytes((size < PAGELIST_LIMIT) ? ((size > CURSOR_STACK_SIZE) ? size : CURSOR_STACK_SIZE)
                                                        : PAGELIST_LIMIT);
  dml_t *const dml = osal_malloc(bytes);
  if (likely(dml)) {
    dml->length = 0;
#ifdef osal_malloc_usable_size
    bytes = osal_malloc_usable_size(dml);
#endif /* osal_malloc_usable_size */
    dml->limit = dml_bytes2size(bytes);
    ASSERT(dml->limit >= size);
  }
  return dml;
}

static __cold dml_t *dml_extend(dml_t *dml) {
  if (!dml)
    return dml_alloc(MDBX_PNL_INITIAL);
  if (unlikely(dml->limit == PAGELIST_LIMIT))
    return nullptr;
  size_t size = (dml->limit < MDBX_PNL_INITIAL * 42) ? dml->limit + dml->limit : dml->limit + dml->limit / 2;
  size_t bytes = dml_size2bytes((size < PAGELIST_LIMIT) ? size : PAGELIST_LIMIT);
  dml = osal_realloc(dml, bytes);
  if (likely(dml)) {
#ifdef osal_malloc_usable_size
    bytes = osal_malloc_usable_size(dml);
#endif /* osal_malloc_usable_size */
    dml->limit = dml_bytes2size(bytes);
  }
  return dml;
}

da_t *dml_append(dml_t **pdml, pgno_t key) {
  dml_t *dml = *pdml;
  ASSERT(!dml || dml->length <= dml->limit);
  if (unlikely(!dml || dml->length == dml->limit)) {
    dml = dml_extend(dml);
    if (unlikely(!dml))
      return nullptr;
    *pdml = dml;
  }
  ASSERT(dml->length < dml->limit);

  da_t *dm = &dml->items[dml->length++], init = {.key_or_pgno = key};
  *dm = init;
  return dm;
}

#define MDBX_DML_EXTRACT_KEY(ptr) (P_INVALID - (ptr)->key_or_pgno)
RADIXSORT_IMPL(dm, da_t, MDBX_DML_EXTRACT_KEY, MDBX_DML_PREALLOC_FOR_RADIXSORT, 0)

#define DM_SORT_CMP(first, last) ((first).key_or_pgno > (last).key_or_pgno)
SORT_IMPL(dm_sort, false, da_t, DM_SORT_CMP)

void dml_sort(dml_t *dml) {
  if (likely(dml->length < MDBX_RADIXSORT_THRESHOLD) || unlikely(!dm_radixsort(dml->items, dml->length)))
    dm_sort(dml->items, dml->items + dml->length);
}

#define DM_SEARCH_CMP(dm, page) ((dm).key_or_pgno > (page))
SEARCH_IMPL(dm_bsearch, da_t, pgno_t, DM_SEARCH_CMP)

da_t *dml_search(dml_t *dml, pgno_t pgno) {
  const da_t *const begin = dml->items;
  const da_t *const it = dm_bsearch(begin, dml->length, pgno);
  const da_t *const end = begin + dml->length;
  ASSERT(it >= begin && it <= end);
  if (it != begin)
    ASSERT(DM_SEARCH_CMP(it[-1], pgno));
  if (it != end)
    ASSERT(!DM_SEARCH_CMP(it[0], pgno));
  return (da_t *)it;
}
