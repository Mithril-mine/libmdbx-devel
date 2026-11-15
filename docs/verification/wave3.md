# Wave 3 merged report — V3 page machine (A), cursor adversarial (B), pairs-3 (C)

Branch `verification/master-audit` @ `fa99524a`. Full per-stream details:
stream A ledger below; stream B — `doc-additions-v5-b.md`; stream C —
`doc-additions-v3c.md`. Repro harnesses: `repro/v3-page-machine-probe.c`
(8 probes, PASS; includes the V-039 counterexample with fix verification),
`repro/v5b-estimate-past-end-dual.c`, `repro/v5b-empty-tree-nav.c`,
`repro/v5b-multiple-batch.c`, **`repro/v3c-put-dangling-cursor.c` (ASAN
stack-use-after-return, empirically confirmed)**.

**Registry note:** streams again used intersecting local V-ids; canonical
renumber: V-037..V-048 (wave-2 canonical ended at V-036). KB keys are slugs —
unaffected.

---

## Stream C findings (the wave's most severe)

### V-037 (C F-C2) — HIGH, empirically confirmed: `mdbx_put` leaves a dangling registered cursor
`api-txn-data.c`: `mdbx_put` registers its stack `cursor_couple_t` in
`txn->cursors[dbi]` (:282-283), but the MDBX_CURRENT size-precheck does a
bare `return MDBX_BAD_VALSIZE` at :301, **bypassing the unregister at :311**.
The dead frame stays linked and is dereferenced by any later cursor op
(m3 loops, `page_touch_unmodifiable`) and by `txn_done_cursors` at txn end.
Repro: DUPSORT+DUPFIXED+INTEGERDUP, key with ≥2 dups, `mdbx_put(MDBX_CURRENT)`
with an out-of-bounds value length, then `mdbx_del` in the same txn →
**ASAN stack-use-after-return on the dead `mdbx_put` frame** (clang build;
gcc build works but ASAN UAR does not report). Fix: unregister before the
early return (or use the exit ladder). Derived review rule INV-CC4: every
explicit registration must pair on every exit.

### V-038 (C F-C1) — medium, build break: `MDBX_ENABLE_BUNCHES_REMOVAL=0` does not compile
Commit 86eb2a15 renamed `cursor_pop`→`cursor_eject` and fixed
tree-cutoff.c:197/api-copy.c:284 but missed **tree-ops.c:25 and :87** (the
`#else` branch of `tree_drop`). Reproduced with
`gcc -fsyntax-only -DMDBX_ENABLE_BUNCHES_REMOVAL=0`. Default/amalgamated
builds unaffected. Rest of pair 1 (tree-ops → cursor-mut/tree-cutoff):
11 runtime sites verified CONSISTENT (enroot/undo pairing, root-collapse
shift bounded by `top+stash`, V-020 confirmed).

### V-047 (C F-C4) — low, V-028 family extension
The **nested-abort** site (`nested_undo` → `dbi_update(false)`,
txn-nested.c:501) also early-returns on dbi_lock failure — rename-undo chain
leaks with the freed nested txn; rename not rolled back (kvs/MAIN desync).
Gated on mutex failure + some dbi in DBI_CREAT.

### Pair 2 (walk.c → chk-walk.c + api-copy.c + defrag.c) — CONSISTENT
Third consumer (defrag.c:280/919) found and verified. Positive result: the
walk's unvalidated `page_numkeys(sp)`/`page_node(sp,ii)` loops are safe only
because `walk_tbl` unconditionally arms `z_pagecheck` → full `page_check`
sub-page validation (**INV-CC1**, load-bearing).

### Pair 4 (tree-search → api-range-estimate/api-txn-data) — arithmetic CONSISTENT
`mdbx_estimate_range` consumes `cursor_diff` (ptrdiff_t), **not**
`cursor_distance_at` — V-035's size_t wrap is not on this path (R-C2); no raw
user-length memcpy remains. But the past-end dual encoding resurfaces at the
estimate layer → V-043 (stream B).

---

## Stream A — V3 page machine (statement-level; full ledger in the agent report)

Verdict census: page-ops.h/page-ops.c/page-get.c/page-iov.c/node.c —
PROVED/PROVED·A per function; **page_split (567 lines) verified in full**:
dupfix reserve branches self-bounding (LEM-V3-2, 5.1M-state enumeration),
tmp-scan accounting identity (LEM-V3-3), tmp-return memcpy exactness via the
node-stacking invariant (LEM-V3-4), every bailout sets MDBX_TXN_ERROR (P5).
`page_check`: type case-split complete; minimum-space formula proven
(LEM-V3-6, branch-exact; leaf slack = one zero-key node caught per-node);
unchecked fields enumerated and owned (INV-P6). `is_tmp` signature
collision-freedom machine-checked (LEM-V3-7).

### Stream A findings
| V-id | file:line | severity | essence |
|---|---|---|---|
| V-039 | page-split.c:507-519 | low (formal UB) | Root-split other-cursor shift is bounded by the OLD tree height, not `m3->top`: at meta-validate-maximal height writes `m3->pg[CURSOR_STACK_SIZE]` — one past `pg[]`, aliasing `ki[0]`, immediately overwritten (benign in practice, formally UB); commits an unopenable height. Machine-checked (PROBE-1). Fix: bound by `m3->top` |
| V-040 | page-ops.c:770-786; page-iov.c:157 | low (checking) / info | (a) unsatisfiable `eASSERT` in `recalculate_subpage_thresholds` branch 1 — user-reachable via `MDBX_opt_subpage_*`; (b) single large-page write can exceed MAX_IO_BYTES — guarded only by checking-build assert; release-safe via osal clamp+partial-write loop (SUSPECT contract) |
| V-041 | node.c:210-215/282-287/136-145 | info | Hardening asymmetry: `node_del`'s memmove-underflow guard is checking-only and its primary guard is VALIDATION-gated, while sub-pages got a hot-path `subpage_geometry_ok` — crafted main-page offset < upper reaches an underflowed memmove in default builds (trust-model A7 documented) |
| V-042 | page-ops.c:128; cursor-mut.c:335/342; gc-put.c:1297 | info (latent) | `page_unspill` PROBLEM and shadowed-large PROBLEM paths do not set `MDBX_TXN_ERROR` (unlike page_touch_unmodifiable); unreachable for legit stamps today |

New lemmas: LEM-V3-1 (iov-page disk-stamp normalizer — closes the PAIR-N9
front/disk/coherency loop; load-bearing), LEM-V3-2..V3-10 (see agent ledger);
assumptions A1..A7 registered.

---

## Stream B — cursor subsystem adversarial (full ledger in doc-additions-v5-b.md)

Verdict census: every function ATTACKED→held or NOT-ATTACKED; **no
memory-safety refutation in the cursor subsystem**; one new release-visible
wrong-result finding. Key held results: MULTIPLE batch failure accounting
(repro E1-E3 — the V-038-not-a-bug hypothesis refuted: done counts successes
only, prefix transactional); convert_to_subtree crafted-ksize held (INV-CB2:
valsize_max(DUPFIXED) < leaf_nodemax < UINT16_MAX — uint16 casts cannot
truncate); sub-page growth overflow held (INV-CB1: subpage_limit ≤
leaf_nodemax−NODESIZE ⇒ mp->upper ≪ UINT16_MAX); full-collapse navigation
held (INV-CB6, repro); tree_search crafted-unsorted-branch: wrong-but-bounded
results only (sortedness unchecked on-disk in release — chk-only);
dupsort_setup validates N_TREE records and N_DUP subpages before use.

### Stream B findings
| V-id | file:line | severity | essence |
|---|---|---|---|
| V-043 | cursor-seek.c:100+115/152+280/676 vs api-range-estimate.c:13/323 | low | past-end dual encoding → `mdbx_estimate_distance` = −1, `mdbx_cursor_distance(deepness=0)` = −2 (size_t wrap), `mdbx_estimate_range` = 0 for the same logical range — silent under asserts; repro v5b-estimate-past-end-dual.c (V-032/V-035 family, unified as INV-CB5) |
| V-044 | cursor-seek.c:748-764 | info | dup-counting distance over multi-page nested trees misses the intra-hive rank discount (acknowledged in-code) |
| V-045 | cursor-mut.c:887-888; cursor-setup.c | doc | clc is env-global → cross-txn sticky pin of lmin/lmax (contract enforcement, formally racy but aligned-size_t consistent) — INV-CB4 |
| V-046 | cogs.h:318 vs real leaf2 pages | info | real leaf2 **pages** lack the release-mode geometry check that sub-pages have — mdbx_chk-only today (hardening note) |

New invariants: INV-CB1..CB6 (sub-page growth bound; dupfix size bound;
poisoned-txn cursor containment; clc pin discipline; past-end dual-encoding
disease unified; full-collapse navigation).

---

## Cross-stream checks (wave 3)

- B's V-043 (estimate layer) and C's V-032/V-035 (wave 2, seek layer) — same
  root cause (past-end dual encoding), unified as INV-CB5; C's R-C2 confirms
  estimate_range itself consumes cursor_diff, so the divergence is confined
  to the two rank APIs.
- A's V-039 (root-split shift) and wave-1 C's cursor arithmetic — the
  bound-by-`m3->top` fix also removes the V-019 family concern at this site.
- C's R-C1: `fix_parent` missing TXN_ERROR is compensated by page_split's
  bailout (every bailout sets MDBX_TXN_ERROR — A's per-branch verification).
- No verdict contradictions.

## KB records (streams wrote directly; 20+ records + links this wave)

A: `bug:page-split:root-shift-oob`, `bug:node:crafted-offset-guard`,
`bug:page-ops:subpage-threshold-assert`, `bug:page-iov:max-io-bytes`,
`bug:page-ops:unspill-error-flag`, `fact:verification:iov-page-disk`,
`page-get-unchecked`, `page-split-splitpoint`, `page-check-space`,
`tmp-signature`, `page-check-outofmap` (+10 links).
B: `bug:cursor:estimate-past-end`, `bottleneck:cursor:distance-multipage-hive`,
`fact:verification:cursor-batch-accounting`, `full-collapse-nav`,
`cursor-bound-invariants`, `clc-pin-discipline`, `crafted-page-rebalance`.
C: `bug:api-txn-data:put-dangling-cursor`, `bug:tree-ops:bunches-removal-build`,
`bug:cursor-mut:fix-parent-cursor`, `bug:txn:rename-undo-nested`,
`fact:verification:walk-containment-chain`, `dbi-state-machine`,
`temp-cursor-self`, `tree-ops-contracts`.

## Metrics after wave 3

- Functions with verdicts: ≈230 (w2) + ~60 (A page machine) + ~25 (B cursor) + 4 pair-groups (C) ≈ **320+**
- Conditions verified: ≈2,400 + ~900 ≈ **~3,300 / 9,711**
- Cumulative REFUTED defects: 9 (V-002 critical, V-003 high, **V-037 high NEW**, V-004/V-038 medium, V-023/24/25/39 low) + build-break V-038 medium + refuted-as-non-bug risks (R7/V-029, V-029-not-a-bug B, R-C1/R-C2)
- Coverage note: cursor files (V5) now hold two full independent lenses (C contracts w1/w2 + B adversarial w3 with per-function ledger) — A-lens pass deemed redundant; recorded in the tranche table.
