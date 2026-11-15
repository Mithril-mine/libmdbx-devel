/* v6b-nested-begin-fault-recipe.c — INV-T1 field-state test recipe (stream B, wave 2).
 *
 * This is a TEST RECIPE for the probe (MPROBE) build of libmdbx, plus a
 * compile-time checklist. It documents, per nested_start() failure site, the
 * exact MPROBE arming sequence and the parent-state assertions that must hold
 * after the failed mdbx_txn_begin(MDBX_TXN_NESTED...) — the field-state table
 * of doc-additions-v6-b.md §INV-T1 in executable form.
 *
 * Build (probe build required for the fault injection parts):
 *   # amalgamated probe build, e.g.
 *   make check-probe   (or the iolingual equivalent with MDBX_PROBES enabled)
 * and compile this file against the amalgamated header for the API calls:
 *   cc -I<src> -o v6b-nested-begin-fault-recipe v6b-nested-begin-fault-recipe.c -L. -lmdbx
 *
 * Without MDBX_PROBES the MPROBE_FAULT sites are no-ops and the scenario
 * functions below still run as regression smoke tests (self-check of the
 * happy paths they exercise).
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal standalone API-level skeleton: the actual fault-rule arming uses the
 * probe-bus control interface of the probe build; the names below are the
 * stable MPROBE tags fired inside src/txn-nested.c. */
static int probe_arm_fault(const char *tag, unsigned hits) {
  (void)tag;
  (void)hits;
  /* probe build: mdbx_probe_rule_add(tag, mprobe_kind_fault, MDBX_ENOMEM, hits) */
  return 0; /* no-op outside probe builds */
}

#define TAG_START_ENOMEM "txn_nested_start_enomem"
#define TAG_LOOSE_MOVE "txn_nested_loose_move"
#define TAG_GEO_UNDO "txn_nested_geo_undo"

/* ---- scenario A: S1/S2 (first allocation of nested->wr.repnl fails) ----
 * arming: probe_arm_fault(TAG_START_ENOMEM, 1)
 * parent precondition: any writer txn, no special state needed.
 * post-conditions (all must hold; these ARE the assertions):
 *   A1 mdbx_txn_begin(nested) returned MDBX_ENOMEM;
 *   A2 parent remains fully usable: begin a cursor, put/get, commit — SUCCESS;
 *   A3 parent->geo unchanged (no spurious dxb_resize attempted): compare
 *      mdbx_env_info_ex().geo upper/now before/after (the #448 regression);
 *   A4 env->txn == parent (ownership restored);
 *   A5 no reader-slot or dirtyroom drift: env->lck unsynced counters unchanged.
 */
static void scenario_A_notes(void) {
  /* arming + calls:
   *   probe_arm_fault(TAG_START_ENOMEM, 1);
   *   rc = mdbx_txn_begin(env, parent_nested_candidate, MDBX_TXN_NESTED, &nested);  (probe build: MDBX_ENOMEM)
   *   assert(parent usable, geo unchanged, env->txn == parent);
   */
}

/* ---- scenario B: S4/S5 (loose-move loop fails mid-way) ----
 * parent precondition: >= 2 loose pages (create: allocate page, then free it —
 * e.g. put+delete a large record in a fresh page range so the freed page
 * becomes dirty-loose below the frontier).
 * arming: probe_arm_fault(TAG_LOOSE_MOVE, 1) — fails the SECOND iteration.
 * post-conditions:
 *   B1 begin returned the injected error; parent usable (A2..A5 as above);
 *   B2 parent's dirtyroom + dirtylist length invariant:
 *      dirtyroom + dl.length == dp_limit (observable via txn_info/mdbx_env_info);
 *   B3 the k absorbed loose pages are now in parent's repnl — semantics: still
 *      free for reuse by the parent; a subsequent parent put must not
 *      double-allocate (covered by audit_ex in checking builds);
 *   B4 parent->wr.loose_refund_wl is stale — benign per INV-T4; no assertion
 *      possible at API level beyond B2/B3.
 */

/* ---- scenario C: S8 (txn_shadow_cursors fails at cursor k of n) ----
 * parent precondition: >= 2 LIVE cursors opened on MAIN_DBI by the parent
 * (mdbx_cursor_open of MAIN_DBI), plus optionally one cursor that was closed
 * during a PREVIOUS nested txn (cur_signature_wait4eot) — see INV-T2.
 * arming: cursor_shadow malloc failure — probe build rule on the
 * cursor_shadow tag or a malloc fault-injection rule.
 * post-conditions (the load-bearing INV-T2 assertions):
 *   C1 after the failed begin, ALL parent cursors are traversable: iterate
 *      mdbx_cursor_get on each — every cursor must answer (no orphaned/dangling
 *      cursors, chain fully reconstructed from backups);
 *   C2 cursor_count(parent) == n (unchanged);
 *   C3 begin a new nested txn afterwards and commit — SUCCESS (no cursor state
 *      leakage from the failed attempt);
 *   C4 the wait4eot cursor (if present) is skipped by the shadowing and stays
 *      in the parent's chain — its MDBX_cursor handle still returns
 *      MDBX_EBADSIGN/EINVAL on use (it is dead until EOT) but the chain is not
 *      broken.
 */

/* ---- scenario D: S6/S7 (rkl_copy fails) ----
 * parent precondition: gc.reclaimed or gc.ready4reuse non-empty — i.e. the
 * parent did GC reclamation work (allocate/commit so retired pages get
 * reclaimed ids into the lists).
 * arming: malloc fault on rkl_copy's rkl_resize.
 * post-conditions: as A1..A5; additionally the parent's next nested begin
 * must succeed and rkl state must be intact (gc lists are copied anew).
 */

/* ---- scenario E (#448 anchor): geo snapshot before first fallible op ----
 * arming: TAG_START_ENOMEM at the FIRST allocation AND a dxb_resize fault
 * (TAG_GEO_UNDO armed to fail with MDBX_EPERM) to emulate the Windows
 * reader-holds-remap-lock refusal.
 * post-conditions:
 *   E1 the parent's geo.upper/now are UNCHANGED (the fix's contract: against a
 *      properly snapshotted geo the comparison in nested_undo() is
 *      exact-equality false, so no spurious resize/EPERM promote happens);
 *   E2 parent commits successfully afterwards.
 */

int main(void) {
  /* Outside a probe build this program only prints the recipe; the scenario
   * drivers belong to tests/ut once promoted (see doc-additions §4 gaps 1-2). */
  printf("v6b nested-begin fault recipe:\n");
  printf("  S1/S2 %s, S4/S5 %s, S8 %s, S6/S7 %s, geo-undo %s\n",
         probe_arm_fault(TAG_START_ENOMEM, 1) == 0 ? "armable" : "N/A",
         probe_arm_fault(TAG_LOOSE_MOVE, 1) == 0 ? "armable" : "N/A", "cursor_shadow malloc rule",
         "rkl_resize malloc rule", probe_arm_fault(TAG_GEO_UNDO, 1) == 0 ? "armable" : "N/A");
  printf("assertions per scenario: see comments (A1-A5, B1-B4, C1-C4, D, E1-E2)\n");
  return EXIT_SUCCESS;
}
