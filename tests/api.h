/// \copyright SPDX-License-Identifier: Apache-2.0
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2015-2026

#pragma once

/* The testing API: introspection, testing and debugging extensions.
 *
 * This header is included from the end of mdbx.h (between dist-cutoff
 * markers, under #if MDBX_ENABLE_TESTS), so every consumer of the public
 * header sees these declarations in development builds. The implementation
 * lives in tests/api.c, which is compiled into the library itself
 * (src/alloy.c includes it under #if MDBX_ENABLE_TESTS, and the non-alloy
 * CMake source list appends it), so the wrappers can reach internal
 * interfaces without link-time acrobatics. The amalgamation (`make dist`)
 * strips every dist-cutoff region and the MDBX_ENABLE_TESTS default, so
 * none of this is present in the distributed sources - see tests/AGENTS.md
 * for the rules of adding new entries.
 *
 * Naming rules: every public function is prefixed with `mdbx_tst_`, every
 * private (implementation-internal) function/type with `tst_`; the public
 * mirror constants follow the same MDBX_TST_* pattern. As this API grows,
 * the monolithic tests/api.c will be split into per-area parts in the
 * tests/api/ directory. */

#ifdef __cplusplus
extern "C" {
#endif

/** \addtogroup c_testing
 * \brief The testing API: introspection, testing and debugging extensions
 * (development builds only).
 *
 * Everything here exists exclusively for the test suite, debugging and
 * introspection. The API is never present in the amalgamated
 * (distributed) sources, is not subject to the ABI stability rules and
 * must stay inert unless explicitly used by a caller. */

/* Options for mdbx_tst_walk_pages(), mirroring the internal
 * walk_options_t (asserted to match at compile time in tests/api.c). */
#define MDBX_TST_WALK_DONT_CHECK_KEYS_ORDERING 0x01u
#define MDBX_TST_WALK_DONT_WALK_GC 0x02u
#define MDBX_TST_WALK_DONT_WALK_MAIN 0x04u
#define MDBX_TST_WALK_DONT_CHECK_REVISIT 0x08u

/* The page kinds reported by the walk visitor, mirroring the internal
 * page_type_t (asserted to match at compile time in tests/api.c). */
enum MDBX_tst_page_type {
  MDBX_TST_PAGE_BROKEN = 0,
  MDBX_TST_PAGE_LARGE = 0x04,
  MDBX_TST_PAGE_BRANCH = 0x01,
  MDBX_TST_PAGE_LEAF = 0x02,
  MDBX_TST_PAGE_DUPFIX_LEAF = 0x20,
  MDBX_TST_PAGE_SUB_LEAF = 0x42,
  MDBX_TST_PAGE_SUB_DUPFIX_LEAF = 0x60,
};

/* The cursor zone flags reported by mdbx_tst_cursor_state(),
 * mirroring the internal cursor zone byte (asserted to match at compile
 * time in tests/api.c; see the comment before enum cursor_state in
 * src/cursor.h for the state-machine contract). */
#define MDBX_TST_Z_INNER 0x01u
#define MDBX_TST_Z_GCU_PREPARATION 0x02u
#define MDBX_TST_Z_FRESH 0x04u
#define MDBX_TST_Z_AFTER_DELETE 0x08u
#define MDBX_TST_Z_INNER_TAIL_DELETED 0x10u
#define MDBX_TST_Z_EOF_SOFT 0x20u
#define MDBX_TST_Z_EOF_HARD 0x40u
#define MDBX_TST_Z_HOLLOW 0x80u

/** \brief The visitor of mdbx_tst_walk_pages().
 *
 * Every page of the snapshot is delivered exactly once. Returning a
 * non-zero value aborts the walk and becomes the return code of
 * mdbx_tst_walk_pages().
 *
 * \param ctx              The caller's context, passed through.
 * \param pgno             The page number; for a sub-page visit this is the
 *                         enclosing leaf's number.
 * \param number_pages     1 for ordinary pages, the contiguous span length
 *                         for large/overflow chains, 0 for sub-page visits
 *                         (a dup bush stored inside a node - excluded from
 *                         the page accounting).
 * \param deep             The tree depth: 0 at the root, +1 per branch
 *                         level; sub-trees and large chains are +1 deeper.
 * \param table_name       "GC", "MAIN" or the named table's key bytes.
 * \param table_name_len   The length of table_name in bytes.
 * \param table_nested     Non-zero when the visit belongs to a nested
 *                         (dupsort sub-tree) table.
 * \param page_size        The page size in bytes; the span size in bytes
 *                         for large pages; the node size for sub-pages.
 * \param page_type        One of MDBX_TST_PAGE_*.
 * \param page_txnid       The page's MVCC stamp.
 * \param page_errcode     The per-page corruption verdict found while
 *                         classifying (MDBX_SUCCESS for a healthy page).
 * \param nentries         The number of keys/nodes (1 for large pages).
 * \param payload_bytes    The key+value payload bytes of the page.
 * \param header_bytes     The page header + node headers + entries bytes.
 * \param unused_bytes     The free space of the page (align included).
 * \param parent_pgno      The referring page, 0 for tree roots. */
typedef int MDBX_tst_walk_func(void *ctx, const MDBX_txn *txn, size_t pgno, unsigned number_pages, unsigned deep,
                               const char *table_name, size_t table_name_len, int table_nested, size_t page_size,
                               int page_type, uint64_t page_txnid, int page_errcode, size_t nentries,
                               size_t payload_bytes, size_t header_bytes, size_t unused_bytes, size_t parent_pgno);

/** \brief Walks every page of the snapshot through the visitor.
 *
 * A direct exposure of the internal walk engine (the same one
 * mdbx_env_chk() drives): every legal page shape must satisfy
 * payload_bytes + header_bytes + unused_bytes == page_size, every page is
 * visited exactly once (unless MDBX_TST_WALK_DONT_CHECK_REVISIT),
 * and the sum of the visited spans plus the three meta pages equals
 * first_unallocated.
 *
 * \param txn      A read transaction to walk.
 * \param visitor  The visitor (mandatory).
 * \param ctx      Passed through to the visitor.
 * \param options  A bitwise OR of MDBX_TST_WALK_*, or 0.
 * \return MDBX_SUCCESS, the visitor's non-zero abort code, or an error. */
LIBMDBX_API int mdbx_tst_walk_pages(MDBX_txn *txn, MDBX_tst_walk_func *visitor, void *ctx, unsigned options);

/** \brief Overrides the mmap limit (globals.mmap_limit) for testing.
 *
 * Affects the code paths that consult the limit (meta validation's
 * MDBX_TOO_LARGE branch, geometry clamps). Dev-only and process-wide:
 * use narrowly around the checked call and restore immediately.
 *
 * \param limit  The limit in bytes to install.
 * \return The previous limit, to pass to mdbx_tst_restore_mmap_limit(). */
LIBMDBX_API uint64_t mdbx_tst_override_mmap_limit(uint64_t limit);

/** \brief Reads the transaction's first-unallocated page number.
 *
 * The exact number of pages in the snapshot: every page below it is either
 * used by a table or listed in the GC, the three meta pages included.
 *
 * \param txn  The transaction to inspect.
 * \return The first unallocated page number, or 0 for a bad handle. */
LIBMDBX_API uint64_t mdbx_tst_geo_first_unallocated(const MDBX_txn *txn);

/** \brief Restores the mmap limit previously overridden.
 *
 * \param previous  The value returned by mdbx_tst_override_mmap_limit(). */
LIBMDBX_API void mdbx_tst_restore_mmap_limit(uint64_t previous);

/** \brief Reads the internal cursor state (the zone byte, stack top, index).
 *
 * Exposes the cursor state machine for characterization: the zone flags
 * (MDBX_TST_Z_*), the stack top (negative for a poor/hollow cursor)
 * and the current index at the top level.
 *
 * \param mc          The cursor to inspect.
 * \param zone_flags  Optional, receives the zone byte (MDBX_TST_Z_*).
 * \param top         Optional, receives the stack top.
 * \param ki_top      Optional, receives the index at the top level
 *                    (0 when the cursor is poor).
 * \return MDBX_SUCCESS or MDBX_EINVAL for a null cursor. */
LIBMDBX_API int mdbx_tst_cursor_state(const MDBX_cursor *mc, unsigned *zone_flags, int *top, unsigned *ki_top);

/** @} end of c_testing */

#ifdef __cplusplus
} /* extern "C" */
#endif
