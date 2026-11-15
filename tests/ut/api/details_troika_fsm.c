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
/// software or the use or dealings in the software.
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
 *    wrong meta page wins).
 *  - pv2pages_verify() (src/cogs.c) exhaustively checks the 16-bit packed
 *    (exponential quantized) page-count round-trip pv2pages/pages2pv for
 *    all 65536 packed values, so the geometry grow/shrink steps stored in
 *    meta pages never drift a page after a save/load cycle.
 *
 * In release builds (MDBX_CHECKING == 0) both proofs are dead code, so this
 * test calls them directly (white-box, the details_rkl.c pattern of
 * compiling the sources into the test unit). Verification campaign item
 * V-001 / SC-93. */

#define debug_log debug_log_sub

#include "../../../src/cogs.c"
#include "../../../src/meta.c"
#include "details-anchors.h"

MDBX_MAYBE_UNUSED __cold void debug_log_sub(int level, const char *function, int line, const char *fmt, ...) {
  (void)level;
  (void)function;
  (void)line;
  (void)fmt;
}

/*-----------------------------------------------------------------------------*/

/* Sanity complement: the troika packer must agree with the reference
 * decision logic for a few hand-checked corner combinations, not just for
 * the exhaustive machine equality above. */
static void corner_cases(void) {
  /* three metas with equal txnids and equal steadiness: all comparisons
   * equal (c == 1) means the troika cannot order them, so such a fully
   * ambiguous combination must carry the non-valid flag */
  const uint8_t ambiguous = meta_cmp2pack(1, 1, 1, false, false, false);
  troika_t troika;
  troika.fsm = 0;
  meta_troika_unpack(&troika, ambiguous);
  if (TROIKA_VALID(&troika) != 0) {
    fprintf(stderr, "fully ambiguous troika must be invalid\n");
    exit(EXIT_FAILURE);
  }
}

int main(int argc, const char *argv[]) {
  (void)argc;
  (void)argv;

  const bool fsm_ok = troika_verify_fsm();
  printf("troika_verify_fsm: %s\n", fsm_ok ? "ok" : "FAILED");
  const bool pv_ok = pv2pages_verify();
  printf("pv2pages_verify: %s\n", pv_ok ? "ok" : "FAILED");

  corner_cases();
  return (fsm_ok && pv_ok) ? EXIT_SUCCESS : EXIT_FAILURE;
}
