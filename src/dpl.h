/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

#include "essentials.h"

static inline size_t dpl_setlen(dpl_t *dl, size_t len) {
  static const page_t dpl_stub_pageE = {INVALID_TXNID,
                                        0,
                                        P_BAD,
                                        {0},
                                        /* pgno */ ~(pgno_t)0};
  assert(dpl_stub_pageE.flags == P_BAD && dpl_stub_pageE.pgno == P_INVALID);
  dl->length = len;
  dl->items[len + 1].ptr = (page_t *)&dpl_stub_pageE;
  dl->items[len + 1].pgno = P_INVALID;
#if MDBX_DPL_CACHE_NPAGES
  dl->items[len + 1].npages = 1;
#endif /* MDBX_DPL_CACHE_NPAGES */
  return len;
}

MDBX_INTERNAL int __must_check_result txn_dpl_alloc(MDBX_txn *txn);

MDBX_INTERNAL void txn_dpl_free(MDBX_txn *txn);

MDBX_INTERNAL dpl_t *txn_dpl_reserve(MDBX_txn *txn, size_t size);

MDBX_INTERNAL __noinline dpl_t *txn_dpl_sort_slowpath(const MDBX_txn *txn);

static inline dpl_t *txn_dpl_sort(const MDBX_txn *txn) {
  tASSERT(txn, (txn->flags & txn_ro_both) == 0);
  tASSERT(txn, (txn->flags & MDBX_WRITEMAP) == 0 || MDBX_AVOID_MSYNC);

  dpl_t *dl = txn->wr.dirtylist;
  tASSERT(txn, dl->length <= PAGELIST_LIMIT);
  tASSERT(txn, dl->sorted <= dl->length);
  tASSERT(txn, dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  return likely(dl->sorted == dl->length) ? dl : txn_dpl_sort_slowpath(txn);
}

MDBX_NOTHROW_PURE_FUNCTION MDBX_INTERNAL __noinline size_t txn_dpl_search(const MDBX_txn *txn, pgno_t pgno);

MDBX_MAYBE_UNUSED MDBX_INTERNAL const page_t *debug_txn_dpl_find(const MDBX_txn *txn, const pgno_t pgno);

MDBX_NOTHROW_PURE_FUNCTION static inline unsigned dp_npages(const dp_t *dp) {
#if MDBX_DPL_CACHE_NPAGES
  assert(likely((dp->ptr->flags & P_LARGE) == 0) ? 1 : dp->ptr->pages == dp->npages);
  return dp->npages;
#else
  return likely((dp->ptr->flags & P_LARGE) == 0) ? 1 : dp->ptr->pages;
#endif /* MDBX_DPL_CACHE_NPAGES */
}

MDBX_NOTHROW_PURE_FUNCTION static inline unsigned dpl_npages(const dpl_t *dl, size_t i) {
  assert(0 <= (intptr_t)i && i <= dl->length);
  return dp_npages(dl->items + i);
}

MDBX_NOTHROW_PURE_FUNCTION static inline pgno_t dpl_endpgno(const dpl_t *dl, size_t i) {
  return dpl_npages(dl, i) + dl->items[i].pgno;
}

MDBX_NOTHROW_PURE_FUNCTION static inline bool txn_dpl_intersect(const MDBX_txn *txn, pgno_t pgno, size_t npages) {
  tASSERT(txn, (txn->flags & txn_ro_both) == 0);
  tASSERT(txn, (txn->flags & MDBX_WRITEMAP) == 0 || MDBX_AVOID_MSYNC);

  dpl_t *dl = txn->wr.dirtylist;
  tASSERT(txn, dl->sorted == dl->length);
  tASSERT(txn, dl->items[0].pgno == 0 && dl->items[dl->length + 1].pgno == P_INVALID);
  size_t const n = txn_dpl_search(txn, pgno);
  tASSERT(txn, n >= 1 && n <= dl->length + 1);
  tASSERT(txn, pgno <= dl->items[n].pgno);
  tASSERT(txn, pgno > dl->items[n - 1].pgno);
  const bool rc =
      /* intersection with founded */ pgno + npages > dl->items[n].pgno ||
      /* intersection with prev */ dpl_endpgno(dl, n - 1) > pgno;
  if (ASSERT_ENABLED()) {
    bool check = false;
    for (size_t i = 1; i <= dl->length; ++i) {
      const page_t *const dp = dl->items[i].ptr;
      if (!(dp->pgno /* begin */ >= /* end */ pgno + npages || dpl_endpgno(dl, i) /* end */ <= /* begin */ pgno)) {
        check = true;
        break;
      }
    }
    tASSERT(txn, check == rc);
  }
  return rc;
}

MDBX_NOTHROW_PURE_FUNCTION static inline size_t txn_dpl_exist(const MDBX_txn *txn, pgno_t pgno) {
  tASSERT(txn, (txn->flags & MDBX_WRITEMAP) == 0 || MDBX_AVOID_MSYNC);
  dpl_t *dl = txn->wr.dirtylist;
  size_t i = txn_dpl_search(txn, pgno);
  tASSERT(txn, (int)i > 0);
  return (dl->items[i].pgno == pgno) ? i : 0;
}

MDBX_INTERNAL void txn_dpl_remove_ex(const MDBX_txn *txn, size_t i, size_t npages);

static inline void txn_dpl_remove(const MDBX_txn *txn, size_t i) {
  txn_dpl_remove_ex(txn, i, dpl_npages(txn->wr.dirtylist, i));
}

MDBX_INTERNAL int __must_check_result txn_dpl_append(MDBX_txn *txn, pgno_t pgno, page_t *page, size_t npages);

MDBX_MAYBE_UNUSED MDBX_INTERNAL bool txn_dpl_check(MDBX_txn *txn);

MDBX_NOTHROW_PURE_FUNCTION static inline uint32_t txn_dpl_age(const MDBX_txn *txn, size_t i) {
  tASSERT(txn, (txn->flags & (txn_ro_both | MDBX_WRITEMAP)) == 0);
  const dpl_t *dl = txn->wr.dirtylist;
  assert((intptr_t)i > 0 && i <= dl->length);
  size_t *const ptr = ptr_disp(dl->items[i].ptr, -(ptrdiff_t)sizeof(size_t));
  return txn->wr.dirtylru - (uint32_t)*ptr;
}

MDBX_INTERNAL void txn_dpl_lru_reduce(MDBX_txn *txn);

static inline uint32_t txn_dpl_lru_turn(MDBX_txn *txn) {
  txn->wr.dirtylru += 1;
  if (unlikely(txn->wr.dirtylru > UINT32_MAX / 3) && (txn->flags & MDBX_WRITEMAP) == 0)
    txn_dpl_lru_reduce(txn);
  return txn->wr.dirtylru;
}

MDBX_INTERNAL void txn_dpl_sift(MDBX_txn *const txn, pnl_t pl, const bool spilled);

MDBX_INTERNAL void txn_dpl_clear(MDBX_txn *txn);
