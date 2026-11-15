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

#ifdef __cplusplus
} /* extern "C" */
#endif
