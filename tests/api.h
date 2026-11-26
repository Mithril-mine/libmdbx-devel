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

/** \brief Runs the internal troika-FSM self-proof (src/meta.c).
 *
 * Exhaustively re-derives the packed troika byte for every combination of
 * the three meta comparison states and asserts the unpacked decisions
 * equal the reference computation. Normally runs only under
 * MDBX_CHECKING; dead code in release builds - this wrapper makes it
 * runnable everywhere. \return true when the proof holds. */
LIBMDBX_API bool mdbx_tst_troika_verify_fsm(void);

/** \brief Runs the internal pv2pages round-trip self-proof (src/cogs.c).
 *
 * Exhaustively checks the 16-bit packed (exponential quantized)
 * page-count round-trip for all 65536 packed values. Normally runs only
 * under MDBX_CHECKING; dead code in release builds - this wrapper makes
 * it runnable everywhere. \return true when the proof holds. */
LIBMDBX_API bool mdbx_tst_pv2pages_verify(void);

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

/* The scan4seq family (src/gc-scan4seq.h): the descending-list search for
 * a page number N whose neighbor N - seq is also in the list - the GC
 * reclaim candidate probe. The differential test
 * (tests/ut/api/details_scan4seq.c) drives every compiled variant through
 * these wrappers and cross-checks them against each other, the internal
 * checker and a written-from-spec oracle. */

/** \brief The identifiers of the scan4seq implementations.
 * \ingroup c_testing */
enum MDBX_tst_scan4seq_variant {
  /** The portable reference loop, always available. */
  MDBX_TST_SCAN4SEQ_FALLBACK = 0,
  /** The SSE2 variant, when compiled for an x86 target with SSE2. */
  MDBX_TST_SCAN4SEQ_SSE2 = 1,
  /** The AVX2 variant, when compiled with AVX2 codegen. */
  MDBX_TST_SCAN4SEQ_AVX2 = 2,
  /** The AVX-512BW variant, when compiled with AVX-512 codegen. */
  MDBX_TST_SCAN4SEQ_AVX512BW = 3,
  /** The runtime CPU dispatch (the resolver the engine itself uses),
   * always available. */
  MDBX_TST_SCAN4SEQ_IMPL = 4
};

/** \brief Reports the scan4seq variants available in this build and on this
 * CPU.
 *
 * A bit `1 << variant` is set when the variant is compiled in and the CPU
 * provides the required features (the runtime probe is done inside the
 * library, mirroring the resolver's own gates). The fallback bit is always
 * set.
 * \ingroup c_testing */
LIBMDBX_API unsigned mdbx_tst_scan4seq_variants_mask(void);

/** \brief Runs one scan4seq implementation over a descending list.
 *
 * \param variant  One of \ref MDBX_tst_scan4seq_variant; a not-available
 *                 variant returns nullptr.
 * \param range    The edge of the descending list: the address just past
 *                 its last element (the scan runs backwards from here).
 * \param len      The number of the list's pages.
 * \param seq      The sequence gap to look for (len - 1 for the neighbor).
 * \return The address of the found list element, or nullptr when no
 *         element pairs with the sequence gap.
 * \ingroup c_testing */
LIBMDBX_API uint32_t *mdbx_tst_scan4seq_variant(unsigned variant, uint32_t *range, size_t len, size_t seq);

/** \brief Runs the internal scan4seq consistency checker over a headed
 * page-number list (the size word at index 0, the descending pages after
 * it).
 *
 * \return The address of the element found by the checker's independent
 *         traversal, or nullptr when it finds no sequence pair.
 * \ingroup c_testing */
LIBMDBX_API const uint32_t *mdbx_tst_scan4seq_checker(const uint32_t *pnl, size_t seq);

/** \brief Runs the exact-length sequence search over a headed page-number
 * list (the size word at index 0, the descending pages after it).
 *
 * Unlike \ref mdbx_tst_scan4seq_variant, this returns only a sequence of
 * exactly `seq + 1` pages: a longer contiguous run is skipped as a whole, so
 * that it stays available to a larger request (the ALLOC_EXACTLY mode of the
 * page-sequence allocator).
 *
 * \return The address of the sequence's tail (the lowest page number), or
 *         nullptr when the list holds no sequence of exactly that length.
 * \ingroup c_testing */
LIBMDBX_API const uint32_t *mdbx_tst_scan4seq_exact(const uint32_t *pnl, size_t seq);

/** \brief Runs the gc_store_retired hook-replacement case matrix (the
 * gc-store-retired-growth-corruption regression scenario): crafted retired lists with an in-window
 * retired-pages growth injected by a cursor_put stub, FIFO/LIFO arms,
 * goodchunk windows - the REAL store loop from src/gc-store-retired.h is
 * compiled into the testing API unit with its callees stubbed.
 *
 * \return The number of failed internal checks (0 = pass).
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_gc_store_retired_cases(void);

/* The GC dense-packing solver (src/gc-dense.h — dissolved back into
 * gc-put.c, the entry is MDBX_INTERNAL there): the closed-form solution
 * covering a `volume` of pages with at most `slots` GC records, cutting
 * chunks from the runs of the page-usage histogram. The differential test
 * (tests/ut/api/details_gc_dense_solve.c) drives it against an independent
 * exact per-run partition DP oracle. */

/** \brief The library's page header size in bytes (the layout constant
 * the GC slot-capacity arithmetic derives from). Pinned to the internal
 * PAGEHDRSZ by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_PAGEHDRSZ 20u

/** \brief Runs the GC dense-packing solver (the gc-dense-solver-unbounded-stall closed form).
 *
 * \param slots           The available GC records/identifiers.
 * \param volume          The pages to cover.
 * \param hist_counts     The page-usage histogram: hist_counts[i] = the
 *                        runs of length i+1.
 * \param hist_end        The histogram boundary (the number of counters).
 * \param first_page      The capacity of a single-page-span chunk.
 * \param other_pages     The per-extra-page capacity of longer chunks.
 * \param solution_counts Out: the chunks per span (at least 31 entries).
 * \param solution_end    Out: the number of used span kinds.
 * \param found           Out: true when the volume is fully covered.
 * \return 0 on a completed solve, -2 when the internal solution
 *         self-check fails (never with correct inputs).
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_gc_dense_pack(size_t slots, size_t volume, const uint32_t *hist_counts, unsigned hist_end,
                                       size_t first_page, size_t other_pages, uint32_t *solution_counts,
                                       unsigned *solution_end, bool *found);

/* The white-box migration seams (issue #519): read-only introspection of the
 * internal state and the build-configuration values that the legacy
 * white-box units reached by including src/internals.h. The derived/layout
 * values are computed inside tests/api.c from the real definitions
 * (drift-proof by construction); the semantic mirrors are the MDBX_TST_*
 * macros pinned by STATIC_ASSERT_MSG in tests/api.c. */

/** \brief The number of the meta pages in the data file (the internal
 * NUM_METAS). Pinned to the internal value by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_NUM_METAS 3

/** \brief Reports the build configuration's MDBX_AVOID_MSYNC option.
 *
 * Non-zero when the persistence of write-mapped dirty pages is routed
 * through explicit writes instead of msync() - the mode where per-commit
 * whole-mapping flushes must not happen.
 * \return The option's value, 0 or 1.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_config_avoid_msync(void);

/** \brief Reports the build configuration's MDBX_MMAP_NEEDS_JOLT option.
 *
 * Non-zero when write-mapped memory needs an explicit MS_ASYNC "jolt" to
 * become visible/persistable (e.g. macOS/BSD/Windows), so one cheap async
 * msync per no-sync commit is the designed behavior there.
 * \return The option's value, 0 or 1.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_config_mmap_needs_jolt(void);

/** \brief Reports the build configuration's MDBX_ENABLE_EXCLUSIVE_FASTPATH
 * option.
 *
 * Non-zero when the MDBX_EXCLUSIVE mode keeps the normal (shareable) i/o
 * handles - the overlapped write-through and dsync ones - and enforces
 * exclusivity by the constant byte-range locks.
 * \return The option's value, 0 or 1.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_config_exclusive_fastpath(void);

/** \brief Reads the data-file mapping's geometry.
 *
 * A read-only introspection of the internal dxb_mmap: the base address,
 * the mapping length (NOT a size of the file nor DB) and the currently
 * recorded/mapped size. The address-range seams (reserving the range right
 * after the mapping, checking that a resize moved nothing) need the base
 * and the limit; the "recorded size must not exceed the mapping" contract
 * compares the current size against the limit.
 *
 * \param env      The environment to inspect.
 * \param base     Optional, receives the mapping base address.
 * \param limit    Optional, receives the mapping length in bytes.
 * \param current  Optional, receives the recorded (mapped) size in bytes.
 * \return MDBX_SUCCESS or MDBX_EINVAL for a null handle.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_env_mapping(const MDBX_env *env, void **base, size_t *limit, size_t *current);

/** \brief Reads the steady checkpoint of the environment's meta-pages.
 *
 * Reports the checkpoint recovery would prefer and whether it is steady — the
 * durability state a defragmentation must not degrade: if the meta-pages held a
 * steady checkpoint when it started, it leaves one behind, and the truncation
 * of the datafile follows a steady checkpoint while it runs.
 *
 * \param env        The environment to inspect.
 * \param txnid      Optional, receives the txnid of the preferred checkpoint.
 * \param is_steady  Optional, receives non-zero when that checkpoint is steady.
 * \return MDBX_SUCCESS or MDBX_EINVAL for a null handle.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_env_steady_checkpoint(const MDBX_env *env, uint64_t *txnid, int *is_steady);

/** \brief Issues the plain grow request of the datafile mapping, exactly as
 * txn_setup_primal() does when a read txn starts with a snapshot larger than
 * the mapping it saw.
 *
 * txn_setup_primal() checks the snapshot against the mapping without the
 * remap-lock, so by the time this request runs, another thread may have grown
 * the mapping past the requested size; such a stale request must not reduce
 * the mapping.
 *
 * \param env        The environment, opened.
 * \param used_pgno  The first unallocated page of the snapshot.
 * \param size_pgno  The datafile size of the snapshot, in pages.
 * \return The result of the internal dxb_resize(), or MDBX_EINVAL for a bad
 *         handle, used_pgno > size_pgno or a size beyond the mapping limit.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_dxb_grow(MDBX_env *env, uint64_t used_pgno, uint64_t size_pgno);

/** \brief Reads the readahead anchor edge in bytes.
 *
 * The internal readahead anchor packs the enable-bit in its LSB and the
 * edge page number shifted left by one; this accessor returns the edge
 * (the anchor shifted right by one) multiplied by the page size - the
 * value the test's "the readahead edge must lie within the mapping"
 * checks compare against the mapping's current size.
 *
 * \param env  The environment to inspect.
 * \return The edge in bytes, or 0 for a null handle / no lock table.
 * \ingroup c_testing */
LIBMDBX_API uint64_t mdbx_tst_env_readahead_bytes(const MDBX_env *env);

/** \brief Reads the environment's data-file i/o handles.
 *
 * The overlapped write-through handle (Windows only: on the other platforms
 * the internal field does not exist and 0 is reported), the dsync handle
 * and the handle the meta pages are written through. The exclusive-fastpath
 * characterization checks that a durable exclusive environment opens the
 * very same handles as a non-exclusive one (the overlapped and the dsync
 * ones) and that meta goes through the dsync handle.
 *
 * \param env            The environment to inspect.
 * \param overlapped_fd  Optional, receives the overlapped write-through
 *                       handle (always 0 on non-Windows platforms).
 * \param dsync_fd       Optional, receives the dsync handle.
 * \param fd4meta        Optional, receives the meta-write handle.
 * \return MDBX_SUCCESS or MDBX_EINVAL for a null handle.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_env_io_fds(const MDBX_env *env, mdbx_filehandle_t *overlapped_fd, mdbx_filehandle_t *dsync_fd,
                                    mdbx_filehandle_t *fd4meta);

/** \brief Reads the write transaction's loose-page accounting in one call.
 *
 * The internal loose counter and the actual length of the loose-pages
 * chain (walked through the engine's own page_next() linkage at the start
 * of the entries): the characterization seam for the loose-counter
 * contract - the counter must always equal the number of the linked
 * pages, at every exit point of a nested-transaction begin.
 *
 * \param txn      The write transaction to inspect.
 * \param counter  Optional, receives the internal loose-pages counter.
 * \param linked   Optional, receives the number of pages actually linked
 *                 in the transaction's loose-pages chain.
 * \return MDBX_SUCCESS or MDBX_EINVAL for a null handle.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_txn_loose_pages(const MDBX_txn *txn, size_t *counter, size_t *linked);

/** \brief The resolved locking scheme of this build.
 *
 * Returns the internal `MDBX_LOCKING` value (1988, 2001, 2008, SYSV or
 * WIN32FILES): the runtime counterpart of the compile-time configuration,
 * for the tests whose scenario depends on the locking flavor.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_config_locking_scheme(void);

/** \brief The offset of the subtable root field inside a `tree_t` record.
 *
 * Computed via `offsetof()` from the live definition, so a layout drift
 * is impossible by construction (the gh0017 corruption diagnostic writes
 * through this offset).
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_tree_offset_root(void);

/** \brief The offset of `rdt_length` inside the lock file's `lck_t`.
 *
 * Computed via `offsetof()` from the live definition (the gh0079
 * corruption diagnostic writes through this offset).
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_lck_offset_rdt_length(void);

/** \brief The offset of `cached_oldest_txnid` inside the lock file's `lck_t`.
 *
 * Computed via `offsetof()` from the live definition (the gh0081
 * corruption diagnostic writes through this offset).
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_lck_offset_cached_oldest_txnid(void);

/** \brief The offset of `rdt_refresh_flag` inside the lock file's `lck_t`.
 *
 * Computed via `offsetof()` from the live definition (the gh0081
 * corruption diagnostic writes through this offset).
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_lck_offset_rdt_refresh_flag(void);

/** \brief The "nothing changed" refresh signature of the reader table.
 *
 * Returns the internal `MDBX_TETRAD('N','o','n','e')` value - computed
 * from the live definition, so a signature drift is impossible by
 * construction.
 * \ingroup c_testing */
LIBMDBX_API uint32_t mdbx_tst_lck_refresh_none(void);

/** \brief Copies the opaque bytes of the environment's write-lock mutex.
 *
 * A read-only introspection for the dead-owner diagnostics: the contents
 * of `lck->wrt_lock` (a process-shared pthread mutex or a semaphore, by
 * the locking scheme) are copied into the caller's buffer, up to `cap`
 * bytes, and the number of actually copied bytes is returned. A negative
 * value means there is no lock mapping to read from.
 *
 * \param env  The environment to inspect.
 * \param buf  The destination buffer, at least `cap` bytes.
 * \param cap  The buffer capacity.
 * \return The number of copied bytes, or a negative value when the
 *   environment has no lock mapping (always so on the WIN32FILES locking
 *   scheme, where no opaque mutex exists).
 * \ingroup c_testing */
LIBMDBX_API ptrdiff_t mdbx_tst_lck_wrt_lock_bytes(const MDBX_env *env, void *buf, size_t cap);

#ifdef __cplusplus
} /* extern "C" */
#endif
