/// \copyright Copyright (c) 2015-2026 Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru>. All Rights Reserved.
///
/// Part of the libmdbx test suite and its supporting infrastructure, which is licensed under the non-free
/// Test Suite Proprietary License: the full license text is in the `tests/LICENCE` file of this repository.
/// The Apache-2.0 license that governs the libmdbx library sources does NOT apply to this file.
///
/// The software is provided "AS IS", without warranty of any kind, express or implied, including but not
/// limited to the warranties of merchantability, fitness for a particular purpose and non infringement.
/// In no event shall the authors or copyright holders be liable for any claim, damages or other liability,
/// whether in an action of contract, tort or otherwise, arising from, out of or in connection with the
/// software or the use or other dealings in the software.
///
/// \author Леонид Юрьев aka Leonid Yuriev <leo@yuriev.ru> \date 2026

/* Activation of the two machine-checked self-proofs that live in the library
 * sources but are only run by mdbx_init() in MDBX_CHECKING builds
 * (src/global.c):
 *
 *  - troika_verify_fsm() (src/meta.c) exhaustively re-derives the packed
 *    troika FSM byte for all 2^3 * 3^3 steady/recent combinations of the
 *    three meta-page comparison states, asserting that unpacking yields the
 *    same recent/prefer_steady/tail/valid/strict decisions as the
 *    independent reference computation (the meta-troika state machine is
 *    the recovery selector after a crash - a silent mismatch there means a
 *    wrong meta page wins). The proof also covers the degenerate
 *    fully-ambiguous combination (all comparisons equal, equal steadiness),
 *    whose packed byte must carry the non-valid flag.
 *  - pv2pages_verify() (src/cogs.c) exhaustively checks the 16-bit packed
 *    (exponential quantized) page-count round-trip pv2pages/pages2pv for
 *    all 65536 packed values, so the geometry grow/shrink steps stored in
 *    meta pages never drift a page after a save/load cycle.
 *
 * In release builds (MDBX_CHECKING == 0) both proofs are dead code, so this
 * test calls them through the testing API (mdbx_tst_troika_verify_fsm(),
 * mdbx_tst_pv2pages_verify()). Activates the troika-fsm-proof-unused machine
 * proof (issue #485). A GoogleTest unit per tests/AGENTS.md §12 (issue #519
 * stage 6): one TEST per proof, no environments, no files. */

#include "mdbx.h"
#include <gtest/gtest.h>

/*-----------------------------------------------------------------------------*/

TEST(details_troika_fsm, troika_verify_fsm) { EXPECT_TRUE(mdbx_tst_troika_verify_fsm()); }

TEST(details_troika_fsm, pv2pages_verify) { EXPECT_TRUE(mdbx_tst_pv2pages_verify()); }
