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
 * (tests/ut/details/details_scan4seq.c) drives every compiled variant through
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
 * (tests/ut/details/details_gc_dense_solve.c) drives it against an independent
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

/* ------------------------------------------------------------------------ */
/* The pnl/rkl/txl algorithm engines (issue #519 stage 4).
 *
 * The deepest white-box characterizations (details_pnl.c++, details_rkl.c++)
 * exercise the pnl/rkl/txl engines through this mirror: thin one-to-one
 * forwards of the live linked functions, opaque instance handles for the
 * internal record types, and narrow accessors for the corruption-pokes
 * that drive the defensive branches. No implementation code is copied -
 * every call reaches the real engine (see tests/AGENTS.md §8). */

/** \brief The mirror of the internal `MAX_TXNID`.
 * Pinned to the live definition by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_MAX_TXNID UINT64_C(0xFFFFFFFEFFFFFFFF)

/** \brief The mirror of the internal `MIN_TXNID`.
 * Pinned to the live definition by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_MIN_TXNID UINT64_C(1)

/** \brief The mirror of the internal `INVALID_TXNID`.
 * Pinned to the live definition by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_INVALID_TXNID UINT64_MAX

/** \brief Opaque storage for an instance of the internal `rkl_t` record.
 *
 * The payload is pinned to the internal type's `sizeof` by a STATIC_ASSERT
 * in tests/api.c, so a layout drift is a build break; the layout knowledge
 * itself stays inside the testing API (see tests/AGENTS.md §8). Instances
 * are created and initialized by \ref mdbx_tst_rkl_init and released by
 * \ref mdbx_tst_rkl_destroy - no engine state is allocated by the handle
 * itself.
 * \ingroup c_testing */
typedef struct MDBX_tst_rkl {
  uint64_t aligned_opaque[16];
} MDBX_tst_rkl_t;

/** \brief Opaque storage for an instance of the internal `rkl_iter_t`.
 *
 * The payload is pinned to the internal type's `sizeof` by a STATIC_ASSERT
 * in tests/api.c. Iterators are produced by \ref mdbx_tst_rkl_iterator and
 * \ref mdbx_tst_rkl_find.
 * \ingroup c_testing */
typedef struct MDBX_tst_rkl_iter {
  uint64_t aligned_opaque[2];
} MDBX_tst_rkl_iter_t;

/** \brief The mirror of the internal `rkl_hole_t` result.
 *
 * The internal type is returned by value from \ref mdbx_tst_rkl_hole and
 * read field-wise by the hole-scan characterizations; the mirror is pinned
 * to the internal type's `sizeof` and field offsets by STATIC_ASSERTs in
 * tests/api.c.
 * \ingroup c_testing */
typedef struct MDBX_tst_rkl_hole {
  uint64_t begin;
  uint64_t end;
} MDBX_tst_rkl_hole_t;

/** \brief Forwards the live rkl_init(): brings a handle to the empty state.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_rkl_init(MDBX_tst_rkl_t *rkl);

/** \brief Forwards the live rkl_destroy(): releases the list's resources.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_rkl_destroy(MDBX_tst_rkl_t *rkl);

/** \brief Forwards the live rkl_clear_and_shrink(): empties the list and
 * collapses an external buffer back to the inplace one.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_rkl_clear_and_shrink(MDBX_tst_rkl_t *rkl);

/** \brief Forwards the live rkl_empty() (the solid-interval emptiness).
 * \ingroup c_testing */
LIBMDBX_API bool mdbx_tst_rkl_empty(const MDBX_tst_rkl_t *rkl);

/** \brief Forwards the live rkl_check(): the defensive validity invariant.
 * \ingroup c_testing */
LIBMDBX_API bool mdbx_tst_rkl_check(const MDBX_tst_rkl_t *rkl);

/** \brief Forwards the live rkl_len(): the number of the list elements.
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_rkl_len(const MDBX_tst_rkl_t *rkl);

/** \brief Forwards the live rkl_lowest().
 * \ingroup c_testing */
LIBMDBX_API uint64_t mdbx_tst_rkl_lowest(const MDBX_tst_rkl_t *rkl);

/** \brief Forwards the live rkl_highest().
 * \ingroup c_testing */
LIBMDBX_API uint64_t mdbx_tst_rkl_highest(const MDBX_tst_rkl_t *rkl);

/** \brief Forwards the live rkl_push(): adds an id, extends solid/list.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_rkl_push(MDBX_tst_rkl_t *rkl, uint64_t id);

/** \brief Forwards the live rkl_pop(): removes and returns the extreme id.
 * \ingroup c_testing */
LIBMDBX_API uint64_t mdbx_tst_rkl_pop(MDBX_tst_rkl_t *rkl, bool highest_not_lowest);

/** \brief Forwards the live rkl_reserve(): grows the list's buffer.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_rkl_reserve(MDBX_tst_rkl_t *rkl, size_t wanna_size);

/** \brief Forwards the live rkl_resize().
 *
 * The file-static engine function is un-hidden (declared in src/rkl.h)
 * for this mirror: the shrink-failure (MDBX_PROBLEM) path and the
 * inplace-buffer collapses are unreachable through \ref
 * mdbx_tst_rkl_reserve, which guards `wanna_size` by the length.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_rkl_resize(MDBX_tst_rkl_t *rkl, size_t wanna_size);

/** \brief Forwards the live rkl_merge(): merges src into dst.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_rkl_merge(const MDBX_tst_rkl_t *src, MDBX_tst_rkl_t *dst, bool ignore_duplicates);

/** \brief Forwards the live rkl_destructive_merge().
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_rkl_destructive_merge(MDBX_tst_rkl_t *src, MDBX_tst_rkl_t *dst, bool ignore_duplicates);

/** \brief Forwards the live rkl_copy(): copies src to a fresh dst.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_rkl_copy(const MDBX_tst_rkl_t *src, MDBX_tst_rkl_t *dst);

/** \brief Forwards the live rkl_destructive_move().
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_rkl_destructive_move(MDBX_tst_rkl_t *src, MDBX_tst_rkl_t *dst);

/** \brief Forwards the live rkl_iterator(): makes a forward/reverse cursor.
 * \ingroup c_testing */
LIBMDBX_API MDBX_tst_rkl_iter_t mdbx_tst_rkl_iterator(const MDBX_tst_rkl_t *rkl, bool reverse);

/** \brief Forwards the live rkl_turn(): the iterator's next element.
 * \ingroup c_testing */
LIBMDBX_API uint64_t mdbx_tst_rkl_turn(MDBX_tst_rkl_iter_t *iter, bool reverse);

/** \brief Forwards the live rkl_left(): elements remaining.
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_rkl_left(MDBX_tst_rkl_iter_t *iter, bool reverse);

/** \brief Forwards the live rkl_find(): positions the iterator at the id.
 * \ingroup c_testing */
LIBMDBX_API bool mdbx_tst_rkl_find(const MDBX_tst_rkl_t *rkl, uint64_t id, MDBX_tst_rkl_iter_t *iter);

/** \brief Forwards the live rkl_contain().
 * \ingroup c_testing */
LIBMDBX_API bool mdbx_tst_rkl_contain(const MDBX_tst_rkl_t *rkl, uint64_t id);

/** \brief Forwards the live rkl_hole(): the next hole interval.
 * \ingroup c_testing */
LIBMDBX_API MDBX_tst_rkl_hole_t mdbx_tst_rkl_hole(MDBX_tst_rkl_iter_t *iter, bool reverse);

/** \brief Forwards the live rkl_extend_solid().
 *
 * The file-static engine function is un-hidden (declared in src/rkl.h)
 * for this mirror: the defensive gap_head/gap_tail branches are exercised
 * only through the crafted inconsistent states that \ref
 * mdbx_tst_rkl_set_solid, \ref mdbx_tst_rkl_set_length and \ref
 * mdbx_tst_rkl_set_inplace provide.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_rkl_extend_solid(MDBX_tst_rkl_t *rkl, uint64_t solid_begin, uint64_t solid_end, uint64_t id);

/** \brief Reports whether the list's buffer is the inplace one.
 *
 * The pointer-identity introspection for the inplace-to-external buffer
 * transitions: a grow moves the buffer out of the handle's inplace
 * storage, a shrink may return it back.
 *
 * \param rkl  The list to inspect.
 * \return True when the list lives in the inplace storage.
 * \ingroup c_testing */
LIBMDBX_API bool mdbx_tst_rkl_is_inplace(const MDBX_tst_rkl_t *rkl);

/** \brief The inplace storage capacity, computed from the live definition.
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_rkl_inplace_capacity(void);

/** \brief Writes both solid-interval bounds (always a pair in the tests).
 *
 * The corruption-poke for the rkl_check/rkl_extend_solid defensive branches:
 * the bounds are meaningless separately, so a single accessor covers the
 * pair (see tests/AGENTS.md §8, the surface design rule).
 *
 * \param rkl    The list to corrupt.
 * \param begin  The new solid_begin value.
 * \param end    The new solid_end value.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_rkl_set_solid(MDBX_tst_rkl_t *rkl, uint64_t begin, uint64_t end);

/** \brief Writes the list_length field (the corruption-poke).
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_rkl_set_length(MDBX_tst_rkl_t *rkl, size_t length);

/** \brief Writes the list_limit field (the corruption-poke for the
 * limit-below-inplace and the external-limit branches of rkl_check).
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_rkl_set_limit(MDBX_tst_rkl_t *rkl, size_t limit);

/** \brief Writes one element of the inplace storage (the corruption-poke).
 *
 * \param rkl    The list to corrupt; `index` must be less than \ref
 *               mdbx_tst_rkl_inplace_capacity.
 * \param index  The element's position.
 * \param id     The new value.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_rkl_set_inplace(MDBX_tst_rkl_t *rkl, size_t index, uint64_t id);

/** \brief Points the list's buffer at the inplace storage.
 *
 * The corruption-poke for the limit-below-inplace branch of rkl_check,
 * which requires an inplace-resident list with a spoiled limit - a state
 * no engine path produces deliberately.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_rkl_list_to_inplace(MDBX_tst_rkl_t *rkl);

/** \brief Steps the iterator's position back by one.
 *
 * The re-dereference semantics check: after a pop that shrinks the list
 * behind a positioned iterator, the stale position must still be
 * serviceable by \ref mdbx_tst_rkl_hole - stepping the position is the
 * only way to reach that path.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_rkl_iter_stepback(MDBX_tst_rkl_iter_t *iter);

/** \brief Forwards the live pnl_intersect().
 * \ingroup c_testing */
LIBMDBX_API bool mdbx_tst_pnl_intersect(const uint32_t *a, const uint32_t *b);

/** \brief Forwards the live pnl_check_lite().
 * \ingroup c_testing */
LIBMDBX_API bool mdbx_tst_pnl_check_lite(const uint32_t *pnl, size_t limit);

/** \brief Forwards the live pnl_check_full().
 * \ingroup c_testing */
LIBMDBX_API bool mdbx_tst_pnl_check_full(const uint32_t *pnl, size_t limit);

/** \brief Forwards the live pnl_merge(): merges src into dst.
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_pnl_merge(uint32_t *dst, const uint32_t *src);

/** \brief Reads the size word of a pnl (the number of the pages).
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_pnl_size(const uint32_t *pnl);

/** \brief Builds a pnl in the caller's buffer, encoding the live layout.
 *
 * The representation constructor for the merge/check characterizations:
 * writes the alloclen word, the size word and the (already ordered by the
 * caller) items exactly as the engine's layout expects - the layout
 * knowledge stays here, not in the tests. The items are copied verbatim,
 * so malformed lists (duplicates, disorder) are constructible for the
 * check-failure paths.
 *
 * \param buf       The caller's buffer, at least `alloclen + 1 + count`
 *                  32-bit words.
 * \param alloclen  The capacity to encode (the merge's alloclen ASSERT is
 *                  exercised through deliberately tight values).
 * \param count     The number of the items, may be zero.
 * \param items     The items to copy, may be null for a zero count.
 * \return The pnl pointer into the buffer.
 * \ingroup c_testing */
LIBMDBX_API uint32_t *mdbx_tst_pnl_make(uint32_t *buf, size_t alloclen, size_t count, const uint32_t *items);

/** \brief The internal `txl_max` length limit, computed from the live
 * definition (build-config dependent, so it is an accessor, not a mirror).
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_txl_max(void);

/** \brief Forwards the live txl_alloc().
 * \ingroup c_testing */
LIBMDBX_API uint64_t *mdbx_tst_txl_alloc(void);

/** \brief Forwards the live txl_free().
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_txl_free(uint64_t *txl);

/** \brief Forwards the live txl_size() inline.
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_txl_size(const uint64_t *txl);

/** \brief Forwards the live txl_alloclen() inline.
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_txl_alloclen(const uint64_t *txl);

/** \brief Forwards the live txl_append().
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_txl_append(uint64_t **ptxl, uint64_t id);

/** \brief Forwards the live txl_contain().
 * \ingroup c_testing */
LIBMDBX_API bool mdbx_tst_txl_contain(const uint64_t *txl, uint64_t id);

/** \brief Forwards the live txl_sort().
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_txl_sort(uint64_t *txl);

/** \brief Forwards the live txl_reserve().
 *
 * The file-static engine function is un-hidden (declared in src/txl.h)
 * for this mirror: the "too long" MDBX_TXN_FULL branch is unreachable
 * through \ref mdbx_tst_txl_append, which guards by the current size.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_txl_reserve(uint64_t **ptxl, size_t wanna);

/** \brief Forwards the live hamming_weight() (the population count).
 *
 * The parameter mirrors the internal function's own `size_t` width, so the
 * callers pass the same-width complements (`~set` of a `size_t` set) as-is -
 * without the implicit widening to a wider parameter (which trips the MSVC
 * C4319 warning; the population count itself would not change).
 * \ingroup c_testing */
LIBMDBX_API size_t mdbx_tst_hamming_weight(size_t v);

/** \brief Reports the internal cursor_couple_t's size and the word bits.
 *
 * The auxiliary diagnostic of the rkl characterization: the model's
 * footprint reference. Both out-params are optional.
 *
 * \param couple_size  Optional, receives sizeof(cursor_couple_t).
 * \param word_bits    Optional, receives the internal MDBX_WORDBITS.
 * \return MDBX_SUCCESS or MDBX_EINVAL for a null couple_size.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_cursor_couple_layout(size_t *couple_size, unsigned *word_bits);

/** \brief Reports whether the probe-bus is compiled into this build.
 *
 * The positive availability query for the probe-driven edge cases: the
 * scenarios check it at runtime instead of compile-time cuts, and skip
 * with a "cannot work here" rationale when the probe-bus is absent.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_probes_active(void);

/** \brief Forwards the live mdbx_probe_ctl() request/reply exchange.
 *
 * The generic probe-bus seam: activation, disarming of the dev-asserts,
 * the alloc-fault injection and the per-site counters are all driven by
 * the request strings; the reply buffer is the caller's. Returns
 * MDBX_ENOSYS when the probe-bus is not compiled into this build.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_probe_ctl(const char *request, char *reply, size_t capacity);

/* ------------------------------------------------------------------------ */
/* The on-disk meta format (issue #519 stage 5).
 *
 * The meta_validate reject-branch corpus and the crafted-meta regressions
 * exercise the meta validation through this mirror: the behavioral seam runs
 * the live meta_validate() over a caller-held page buffer, and the
 * layout-accessor family provides the field-level crafting/inspection of the
 * page header and the meta payload. The layout knowledge (the PAGEHDRSZ
 * offset, the meta_t/geo_t/tree_t field positions and widths, the pv
 * encoding) stays inside the testing API - see tests/AGENTS.md §8. */

/** \brief The mirror of the internal `MDBX_MAGIC` stamp.
 * Pinned to the live definition by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_MAGIC UINT64_C(0x59659DBDEF4C11)

/** \brief The mirror of the internal `MIN_PAGENO`.
 * Pinned to the live definition by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_MIN_PAGENO 3u

/** \brief The mirror of the internal `MAX_PAGENO`.
 * Pinned to the live definition by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_MAX_PAGENO UINT32_C(0x7FFFFFFF)

/** \brief The mirror of the internal `MAX_MAPSIZE64` envelope.
 * Pinned to the live definition by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_MAX_MAPSIZE64 UINT64_C(0x800000000000)

/** \brief The mirror of the internal `P_INVALID` page number.
 * Pinned to the live definition by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_P_INVALID UINT32_C(0xFFFFFFFF)

/** \brief The mirror of the internal `P_META` page-flags bit.
 * Pinned to the live definition by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_PAGE_FLAG_META 0x08u

/** \brief The mirror of the internal `P_LEAF` page-flags bit.
 * Pinned to the live definition by a STATIC_ASSERT in tests/api.c.
 * \ingroup c_testing */
#define MDBX_TST_PAGE_FLAG_LEAF 0x02u

/** \brief Selectors of the 64-bit meta fields read/written as words.
 *
 * The values are the wrapper's own dispatch contract (pinned by a
 * STATIC_ASSERT in tests/api.c per the regulation form).
 * \ingroup c_testing */
enum MDBX_tst_meta_u64_field {
  /** The magic-and-version stamp (the low byte is the data version). */
  MDBX_TST_META_MAGIC_AND_VERSION = 0,
  /** The first phase of the two-phase committed-txnid pair. */
  MDBX_TST_META_TXNID_A = 1,
  /** The second phase of the two-phase committed-txnid pair. */
  MDBX_TST_META_TXNID_B = 2,
  /** The steady-sign union (the calculated sign of a steady meta). */
  MDBX_TST_META_SIGN = 3
};

/** \brief Selectors of the meta's geometry fields.
 *
 * The values are the wrapper's own dispatch contract (pinned by a
 * STATIC_ASSERT in tests/api.c per the regulation form).
 * \ingroup c_testing */
enum MDBX_tst_meta_geo_field {
  MDBX_TST_META_GEO_LOWER = 0,
  MDBX_TST_META_GEO_UPPER = 1,
  MDBX_TST_META_GEO_NOW = 2,
  MDBX_TST_META_GEO_FIRST_UNALLOCATED = 3,
  /** The growth step in the pv encoding (see \ref mdbx_tst_pages2pv). */
  MDBX_TST_META_GEO_GROW_PV = 4,
  /** The shrink step in the pv encoding. */
  MDBX_TST_META_GEO_SHRINK_PV = 5
};

/** \brief Selectors of the meta's two table trees.
 * The values are the wrapper's own dispatch contract (pinned by a
 * STATIC_ASSERT in tests/api.c per the regulation form).
 * \ingroup c_testing */
enum MDBX_tst_meta_tree { MDBX_TST_TREE_GC = 0, MDBX_TST_TREE_MAIN = 1 };

/** \brief Selectors of the tree_t fields the characterizations craft.
 *
 * The values are the wrapper's own dispatch contract (pinned by a
 * STATIC_ASSERT in tests/api.c per the regulation form).
 * \ingroup c_testing */
enum MDBX_tst_meta_tree_field {
  MDBX_TST_TREE_FLAGS = 0,
  MDBX_TST_TREE_HEIGHT = 1,
  MDBX_TST_TREE_ROOT = 2,
  MDBX_TST_TREE_BRANCH_PAGES = 3,
  MDBX_TST_TREE_LEAF_PAGES = 4,
  MDBX_TST_TREE_ITEMS = 5,
  MDBX_TST_TREE_MOD_TXNID = 6
};

/** \brief Runs the live meta_validate() over a caller-held page buffer.
 *
 * The behavioral seam of the meta_validate reject-branch corpus and the
 * crafted-meta regressions: the buffer is interpreted as a whole meta page
 * (the page header and the payload overlay it exactly like a mapped meta
 * page), and the verdict is the engine's own return code. The engine may
 * write the corrected geometry values back into the buffer (the same
 * mutation-back semantics as the internal call sites), so the buffer must
 * be writable and 8-byte aligned (the unaligned u64 access inside requires
 * it).
 *
 * \param env            The OPEN environment: the engine reads the file size
 *                       and the mmap-limit globals from it.
 * \param page_buffer    The caller's page buffer, at least one page size
 *                       bytes.
 * \param meta_number    The meta slot the page claims to be (0..2).
 * \param guess_pagesize Optional, receives the meta's pagesize when the
 *                       pagesize branch latches it (the engine's probe-out;
 *                       the corpus passes null).
 * \return The engine's verdict (MDBX_SUCCESS and the reject-branch codes).
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_meta_validate(MDBX_env *env, void *page_buffer, unsigned meta_number,
                                       unsigned *guess_pagesize);

/** \brief Reads one of the 64-bit meta fields as a word.
 *
 * \param page_buffer  The caller's page buffer.
 * \param field        The field selector.
 * \return The field's 64-bit value, or 0 for a null buffer / bad selector.
 * \ingroup c_testing */
LIBMDBX_API uint64_t mdbx_tst_meta_field_get(void *page_buffer, enum MDBX_tst_meta_u64_field field);

/** \brief Writes one of the 64-bit meta fields as a word.
 *
 * \param page_buffer  The caller's page buffer.
 * \param field        The field selector.
 * \param value        The value to store.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_meta_field_set(void *page_buffer, enum MDBX_tst_meta_u64_field field, uint64_t value);

/** \brief Reads the meta's pagesize field.
 * \ingroup c_testing */
LIBMDBX_API uint32_t mdbx_tst_meta_pagesize_get(void *page_buffer);

/** \brief Writes the meta's pagesize field (the crafting of the pagesize
 * reject branches).
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_meta_pagesize_set(void *page_buffer, uint32_t pagesize);

/** \brief Reads one of the meta's geometry fields.
 * \ingroup c_testing */
LIBMDBX_API uint64_t mdbx_tst_meta_geo_get(void *page_buffer, enum MDBX_tst_meta_geo_field field);

/** \brief Writes one of the meta's geometry fields.
 *
 * The pgno-width fields are narrowed to the internal pgno_t and the pv
 * fields to their 16-bit encoding; the narrowing knowledge stays here.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_meta_geo_set(void *page_buffer, enum MDBX_tst_meta_geo_field field, uint64_t value);

/** \brief Writes one of the table-tree fields (the GC or the main tree).
 *
 * The value is narrowed to the field's internal width (the flags/height to
 * 16 bits, the page counts to pgno_t, the counters to 64 bits).
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_meta_tree_set(void *page_buffer, enum MDBX_tst_meta_tree tree,
                                        enum MDBX_tst_meta_tree_field field, uint64_t value);

/** \brief Re-signs the meta as steady (the 1:1 forward of the live inline).
 *
 * Every crafting case except the sign-mismatch one refreshes the sign so
 * the checksum branch passes and the branch under test decides the
 * verdict.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_meta_sign_as_steady(void *page_buffer);

/** \brief Reads the page header's pgno field.
 * \ingroup c_testing */
LIBMDBX_API uint32_t mdbx_tst_page_get_pgno(const void *page_buffer);

/** \brief Writes the page header's pgno field.
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_page_set_pgno(void *page_buffer, uint32_t pgno);

/** \brief Writes the page header's flags field (the crafting of the
 * page-flags reject branch).
 * \ingroup c_testing */
LIBMDBX_API void mdbx_tst_page_set_flags(void *page_buffer, unsigned flags);

/** \brief Reads the environment's page size.
 * \ingroup c_testing */
LIBMDBX_API unsigned mdbx_tst_env_pagesize(const MDBX_env *env);

/** \brief Reads the environment's data-file size in bytes (0 when the
 * environment has no data file mapped).
 * \ingroup c_testing */
LIBMDBX_API uint64_t mdbx_tst_env_filesize(const MDBX_env *env);

/** \brief Copies a mapped page into the caller's buffer.
 *
 * The snapshotting seam: the baseline of the corpus and the crafted-meta
 * regressions is a real committed meta page copied out of the opened
 * environment's mapping (the same read the engine's pgno2page() does).
 *
 * \param env         The open environment.
 * \param pgno        The page number to read.
 * \param dest        The destination buffer, at least `bytes` long.
 * \param bytes       The number of bytes to copy (typically the page size).
 * \return MDBX_SUCCESS or MDBX_EINVAL for a null handle/buffer.
 * \ingroup c_testing */
LIBMDBX_API int mdbx_tst_env_read_page(const MDBX_env *env, uint32_t pgno, void *dest, size_t bytes);

/** \brief Encodes a page count into the pv form (the live encoder).
 *
 * The growth-step crafting uses the live pv encoding instead of a
 * hand-computed constant, so an encoding drift is impossible by
 * construction.
 * \ingroup c_testing */
LIBMDBX_API uint16_t mdbx_tst_pages2pv(size_t pages);

/** \brief The maximal tree height the engine accepts (the live
 * CURSOR_STACK_SIZE bound, build-dependent).
 * \ingroup c_testing */
LIBMDBX_API unsigned mdbx_tst_cursor_stack_size(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
