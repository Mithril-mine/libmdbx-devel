/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

typedef struct defrag_arc {
  pgno_t key_or_pgno;
  pgno_t parent;
  pgno_t mapped : 31;
  bool engaged : 1;
  pgno_t npages : 31;
  bool gc : 1;
} da_t;

typedef struct defrag_map_list {
  size_t length;
  size_t limit;
  da_t items[1];
} dml_t;

MDBX_MAYBE_UNUSED MDBX_INTERNAL dml_t *dml_alloc(size_t size);
MDBX_MAYBE_UNUSED MDBX_INTERNAL void dml_sort(dml_t *);
MDBX_MAYBE_UNUSED MDBX_INTERNAL da_t *dml_search(dml_t *, pgno_t key);
MDBX_MAYBE_UNUSED MDBX_INTERNAL da_t *dml_append(dml_t **pdml, pgno_t key);

MDBX_MAYBE_UNUSED static inline da_t *dml_search_exact(dml_t *dml, pgno_t pgno) {
  da_t *arc = dml_search(dml, pgno);
  return (arc < dml->items + dml->length && arc->key_or_pgno == pgno) ? arc : nullptr;
}

MDBX_MAYBE_UNUSED static inline void dml_free(dml_t **pdml) {
  if (likely(*pdml)) {
    osal_free(*pdml);
    *pdml = nullptr;
  }
}
