/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

/* The implementation of the testing API; see tests/api.h for the contract
 * and tests/AGENTS.md for the rules. This file is compiled into the
 * library itself: src/alloy.c includes it under #if MDBX_ENABLE_TESTS (so
 * the wrappers reach the internal interfaces inside the same translation
 * unit), and the non-alloy CMake build appends it to the source list.
 *
 * Naming rules: every public function here is prefixed with `mdbx_tst_`,
 * every private (implementation-internal) function/type with `tst_`. As
 * this API grows, the monolithic tests/api.c will be split into per-area
 * parts in the tests/api/ directory. */

#include "../src/internals.h"

/* The public mirror constants must stay in sync with the internal ones;
 * a drift here is a build break, not a silent test skew. */
STATIC_ASSERT_MSG(MDBX_TST_WALK_DONT_CHECK_KEYS_ORDERING == (unsigned)dont_check_keys_ordering, "walk options");
STATIC_ASSERT_MSG(MDBX_TST_WALK_DONT_WALK_GC == (unsigned)dont_walk_GC, "walk options");
STATIC_ASSERT_MSG(MDBX_TST_WALK_DONT_WALK_MAIN == (unsigned)dont_walk_MAIN, "walk options");
STATIC_ASSERT_MSG(MDBX_TST_WALK_DONT_CHECK_REVISIT == (unsigned)dont_check_revisit, "walk options");

STATIC_ASSERT_MSG(MDBX_TST_PAGE_BROKEN == (int)page_broken, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_LARGE == (int)page_large, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_BRANCH == (int)page_branch, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_LEAF == (int)page_leaf, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_DUPFIX_LEAF == (int)page_dupfix_leaf, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_SUB_LEAF == (int)page_sub_leaf, "page type");
STATIC_ASSERT_MSG(MDBX_TST_PAGE_SUB_DUPFIX_LEAF == (int)page_sub_dupfix_leaf, "page type");

STATIC_ASSERT_MSG(MDBX_TST_Z_INNER == (unsigned)z_inner, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_GCU_PREPARATION == (unsigned)z_gcu_preparation, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_FRESH == (unsigned)z_fresh, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_AFTER_DELETE == (unsigned)z_after_delete, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_INNER_TAIL_DELETED == (unsigned)z_inner_tail_deleted, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_EOF_SOFT == (unsigned)z_eof_soft, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_EOF_HARD == (unsigned)z_eof_hard, "cursor zone");
STATIC_ASSERT_MSG(MDBX_TST_Z_HOLLOW == (unsigned)(uint8_t)z_hollow, "cursor zone");

/*----------------------------------------------------------------------------*/

struct tst_walk_ctx {
  MDBX_tst_walk_func *visitor;
  void *ctx;
  MDBX_txn *txn;
};

__cold static int tst_walk_adapter(const size_t pgno, const unsigned number, void *const ctx_, const unsigned deep,
                                   const walk_tbl_t *const table, const size_t page_size, const page_type_t page_type,
                                   const txnid_t page_txnid, const MDBX_error_t err, const size_t nentries,
                                   const size_t payload_bytes, const size_t header_bytes, const size_t unused_bytes,
                                   const size_t parent_pgno) {
  const struct tst_walk_ctx *const wrap = (const struct tst_walk_ctx *)ctx_;
  const char *name;
  size_t name_len;
  if (table->name.iov_base == MDBX_CHK_GC) {
    name = "GC";
    name_len = 2;
  } else if (table->name.iov_base == MDBX_CHK_MAIN) {
    name = "MAIN";
    name_len = 4;
  } else {
    name = (const char *)table->name.iov_base;
    name_len = table->name.iov_len;
  }
  return wrap->visitor(wrap->ctx, wrap->txn, pgno, number, deep, name, name_len, table->nested != nullptr, page_size,
                       (int)page_type, page_txnid, (int)err, nentries, payload_bytes, header_bytes, unused_bytes,
                       parent_pgno);
}

__cold int mdbx_tst_walk_pages(MDBX_txn *txn, MDBX_tst_walk_func *visitor, void *ctx, unsigned options) {
  if (unlikely(!visitor))
    return MDBX_EINVAL;
  const struct tst_walk_ctx wrap = {visitor, ctx, txn};
  return walk_pages(txn, tst_walk_adapter, (void *)&wrap, (walk_options_t)options);
}

/*----------------------------------------------------------------------------*/

__cold static size_t tst_clamp_limit(uint64_t value) { return (value > (uint64_t)SIZE_MAX) ? SIZE_MAX : (size_t)value; }

__cold uint64_t mdbx_tst_override_mmap_limit(uint64_t limit) {
  const uint64_t previous = globals.mmap_limit;
  globals.mmap_limit = tst_clamp_limit(limit);
  return previous;
}

__cold void mdbx_tst_restore_mmap_limit(uint64_t previous) { globals.mmap_limit = tst_clamp_limit(previous); }

/*----------------------------------------------------------------------------*/

__cold bool mdbx_tst_troika_verify_fsm(void) { return troika_verify_fsm(); }

__cold bool mdbx_tst_pv2pages_verify(void) { return pv2pages_verify(); }

__cold uint64_t mdbx_tst_geo_first_unallocated(const MDBX_txn *txn) {
  if (unlikely(!txn))
    return 0;
  return (uint64_t)txn->geo.first_unallocated;
}

__cold int mdbx_tst_cursor_state(const MDBX_cursor *mc, unsigned *zone_flags, int *top, unsigned *ki_top) {
  if (unlikely(!mc))
    return MDBX_EINVAL;
  const MDBX_cursor *const inner = mc;
  if (zone_flags)
    *zone_flags = (unsigned)(uint8_t)inner->flags;
  if (top)
    *top = inner->top;
  if (ki_top)
    *ki_top = (inner->top >= 0) ? (unsigned)inner->ki[inner->top] : 0;
  return MDBX_SUCCESS;
}
