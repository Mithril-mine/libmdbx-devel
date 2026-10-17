/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

typedef struct walk_tbl {
  MDBX_val name;
  tree_t *internal, *nested;
} walk_tbl_t;

typedef int walk_func(const size_t pgno, const unsigned number, void *const ctx, const unsigned deep,
                      const walk_tbl_t *table, const size_t page_size, const page_type_t page_type,
                      const txnid_t page_txnid, const MDBX_error_t err, const size_t nentries,
                      const size_t payload_bytes, const size_t header_bytes, const size_t unused_bytes,
                      const size_t parent_pgno);

typedef enum walk_options {
  dont_check_keys_ordering = 1,
  dont_walk_GC = 2,
  dont_walk_MAIN = 4,
  /* Disable the issue-#82 revisit/cycle guard: the walk then allows re-entering
   * a page more than once (a reconverging DAG or a cycle) and is bounded only
   * by the depth of a single path. This is for the `chk` machinery, which has
   * its own loop detection and intentionally inspects such trees to report or
   * recover them instead of failing with MDBX_CORRUPTED at the first cycle. */
  dont_check_revisit = 8
} walk_options_t;

MDBX_INTERNAL int walk_pages(MDBX_txn *txn, walk_func *visitor, void *user, walk_options_t options);

typedef struct walk_ctx {
  void *userctx;
  walk_options_t options;
  unsigned deep;
  walk_func *visitor;
  MDBX_txn *txn;
  MDBX_cursor *cursor;
  /* A valid copy-on-write b-tree references every page exactly once, so the
   * walk marks each visited page in a one-bit-per-page bitmap and rejects any
   * second visit (a cycle or a reconverging DAG) as corruption, which also
   * bounds the traversal by the number of pages in the file (issue #82). */
  uint8_t *visited;    /* bitset, one bit per page; may be a capped subset */
  size_t visited_size; /* nbytes of the bitmap (0 when not in use) */
  size_t nwalked;      /* walk_pgno() entries made so far */
  size_t walk_limit;   /* entries never legitimately exceed the page count */
} walk_ctx_t;

MDBX_INTERNAL int walk_tbl(walk_ctx_t *ctx, walk_tbl_t *tbl, pgno_t parent_page);
