/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

typedef struct defrag_map_item {
  pgno_t pgno;
  pgno_t parent;
  pgno_t mapped;
  pgno_t npages;
} dm_t;

typedef struct defrag_map_list {
  unsigned length;
  unsigned limit;
  dm_t items[1];
} dml_t;

MDBX_MAYBE_UNUSED MDBX_INTERNAL dml_t *dml_alloc(size_t size);
MDBX_MAYBE_UNUSED MDBX_INTERNAL void dml_sort(dml_t *);
MDBX_MAYBE_UNUSED MDBX_INTERNAL dm_t *dml_search(dml_t *, pgno_t key);
MDBX_MAYBE_UNUSED MDBX_INTERNAL dm_t *dml_append(dml_t **pdml, pgno_t key);

MDBX_MAYBE_UNUSED static inline void dml_free(dml_t **pdml) {
  if (likely(*pdml)) {
    osal_free(*pdml);
    *pdml = nullptr;
  }
}
