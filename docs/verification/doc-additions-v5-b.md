# doc-additions-v5-b — cursor subsystem adversarial verification (stream B, wave 5)

Scope: `src/cursor-mut.c`, `src/cursor-nav.c`, `src/cursor-seek.c`,
`src/tree-search.c` plus the directly coupled helpers
(`node.c`, `tree-ops.c:tree_drop/tree_rebalance`, `cursor-setup.c:cursor_touch/
cursor_dupsort_setup/cursor_copy_position`, `api-range-estimate.c`,
`api-cursor.c`, `cogs.h` size/valsize machinery). Wave-2 cursor findings
(V-029 SET_RANGE past-end dual encoding, V-030/V-033 TO_KEY_EQUAL miss,
V-032/V-035 distance eof_hard rank wrap) are referenced, not duplicated.

Verdict census: all four target files walked line-by-line; 12-point
adversarial checklist executed; 3 executable repros (all PASS on Release AND
live-assert Debug builds); **one new release-visible finding (V-037), one
hypothesis refuted (V-038-not-a-bug), one acknowledged-gap confirmed (V-039),
two doc-level findings (V-040, V-041)**. No memory-safety refutation: every
crafted-page and state-combination attack landed inside an existing guard.

Repros (executable, deterministic):
- `repro/v5b-estimate-past-end-dual.c` — V-037;
- `repro/v5b-empty-tree-nav.c` — full-collapse navigation (attack 6) PASS;
- `repro/v5b-multiple-batch.c` — MULTIPLE batch accounting (attack 1) PASS,
  V-038 hypothesis refuted.

---

## 1. New finding

### V-037 — past-end dual encoding breaks rank arithmetic in the estimate/cursor_distance family (R1 gap, third consumer)

`bug:cursor:estimate-past-end-dual`, importance 0.7 (release-visible wrong
results; no memory unsafety; no abort even with asserts).

Mechanism. `cursor_seek(MDBX_SET_RANGE)` past the end leaves one of two
encodings (V-029): (a) fast path `cursor-seek.c:100+115` —
`ki[top] = nkeys`, `z_eof_soft|z_eof_hard`; (b) sibling-failure
`cursor-seek.c:152+280` (`cursor_sibling_right` NOTFOUND) —
`ki[top] = nkeys-1`, same flags. Three public rank APIs read this state with
three different rank conventions:

| consumer | file | convention | result for (a) | result for (b) |
|---|---|---|---|---|
| `mdbx_estimate_distance` | `api-range-estimate.c:13 cursor_diff` | literal `ki`, `CMP2INT(eof_hard)` only at full-ki tie | **-1** (8-9) | 0 (`CMP2INT(1,1)`) |
| `mdbx_cursor_distance` (deepness=0) | `cursor-seek.c:676 cursor_distance_at` | literal `ki` **plus** `+1` eof_hard adjust when `level+1 == height` | **-2** (8-(9+1) → int → `size_t` wrap → `intptr` -2) | 1 (`CMP2INT` tie at distance level? no — handled by the cmp==0 pre-tiebreak in `api-cursor.c`) |
| `mdbx_estimate_range(begin>past-end, end=LAST)` | fresh begin → encoding (b); end forced `z_eof_hard` (`api-range-estimate.c:323`) | both hard → `CMP2INT(1,1)` | — | **0** |

One logical range (begin = SET_LOWERBOUND past-end, end = LAST row) answers
**-2 / -1 / 0** depending on which internal path the past-end position took
and which API is asked. `mdbx_estimate_range` cannot produce encoding (a)
itself (its begin cursor is always fresh/poor → sibling encoding), but a
user cursor parked by `SET_LOWERBOUND` (fast path, pointed) then fed to
`mdbx_estimate_distance`/`mdbx_cursor_distance` does. Repro asserts all
observed values deterministically.

Aggravating facts:
- silent in checking builds too — `cursor_distance_at`'s wrap and
  `cursor_diff`'s literal-ki read have **no assert on this path**
  (`ASSERT(tdr.distance > 0)` guards only `cursor_distribute`);
- `mdbx_estimate_move` is accidentally safe both ways (`CMP2INT`
  tie-break fires exactly when the source is the last row; the `LAST`
  move-op force-marks `z_eof_hard` on the target, symmetric);
- `cursor_distribute` absorbs the wrap (`tree_diff_level` → INT_MIN →
  `ENODATA`, then `be_poor` + `MDBX_RESULT_TRUE`) — V-035's consumer — so
  only the distance/estimate APIs leak numbers.

Fix direction (not applied): normalize the past-end encoding at seek exit
(e.g. always `ki = nkeys-1` + eof flags), or teach `cursor_diff` /
`cursor_distance_at` the single canonical rank formula
`rank = ki + (eof_hard && level+1 == leaf-level)`. The V-029/032/035/037
family then collapses into one normalization.

### V-039 (confirmed acknowledged gap) — dup-counting distance over multi-page nested trees misses the intra-hive rank discount

`bottleneck:cursor:distance-multipage-hive`, importance 0.4.
`cursor_distance` (`cursor-seek.c:748-764`) applies the intra-hive rank
discount only when `iter->subcur->nested_tree.height <= 1`; the in-code
comment states deeper nested trees "are left with the pre-fix accounting
until the multi-page nested-tree walking defect is resolved". Consequence:
`mdbx_cursor_distance(..., deepness >= height)` over a hive grown beyond a
sub-page over-counts by the rank of begin inside its own hive. Acknowledged
in code; registered here so the obligation is tracked (KB link
`fn:cursor_distance`).

---

## 2. Hypothesis REFUTED (defense holds — documented precisely)

### V-038-not-a-bug — MULTIPLE|NODUPDATA duplicate element: uniform MDBX_KEYEXIST

Hypothesis (checklist attack 1/2 variant): a NODUPDATA element duplicating an
in-hive value mid-batch would take the inner `cursor_put(KEYEXIST)` →
`dupsort_error` re-map to `MDBX_PROBLEM` and poison the txn, while the
element-1 duplicate (eq_fast path, `cursor-mut.c:402-414`) returns a clean
`MDBX_KEYEXIST` — a structural inconsistency. **Refuted by repro
`v5b-multiple-batch.c` (E2a/E2c): both return `MDBX_KEYEXIST`.** Why:
`dupsort_error` (`cursor-mut.c:832-837`) is reachable only through
`if (insert_key) { if (rc != MDBX_SUCCESS) goto dupsort_error; ... }` —
batch elements 2+ have `insert_key == false` (reset at
`cursor-mut.c:814`), so the inner rc is returned **as-is** at line 830.
The PROBLEM re-mapping only fires for batch element 0 / hive-creation
failures. Uniformity holds; no txn poison on mid-batch KEYEXIST.

### Attack 1 (MULTIPLE mid-batch failure) — HELD

`repro/v5b-multiple-batch.c` demonstrates:
- `*batch_dupfix_done` counts exactly the successes (E2a done=1, E2c done=2);
- `data[0].iov_base` advances only after each succeeded element and never
  past the buffer (`done <= given-1` gate at `cursor-mut.c:812`); on the
  last element no advance;
- the partially-inserted prefix belongs to the txn: commit persists exactly
  the done-count elements (E2d: hive {10,20,30,40}), abort discards it
  (E1: get → NOTFOUND after abort);
- the txn survives a mid-batch failure (no `MDBX_TXN_ERROR` from the
  batch loop itself) and the caller may resume from `data[0].iov_base`;
- without NODUPDATA, in-vector duplicates are silently skipped ("the same
  data, nothing to update") and still counted in done (E3: done=5 of 5,
  hive items=3);
- the static `cursor_touch` re-check between elements
  (`cursor-mut.c:821`): a failure there returns `err` with the same
  consistent accounting (advance already done for consumed elements; the
  prefix stays in the txn; MDBX_TXN_FULL-class failures abort the txn at
  the caller). Not dynamically triggered (spill/dirtyroom exhaustion is not
  deterministically reachable without MPROBE fault injection) — static
  verdict only.

### Attack 6 (navigation after full-tree collapse) — HELD

`repro/v5b-empty-tree-nav.c` (small + 300-key height-2 variants): cursor A
parked past-end (`z_eof_hard`) / `z_eof_soft` while cursor B deletes all
rows. Defense chain verified end-to-end:
- `tree_rebalance` empty-root collapse walks `txn->cursors` and applies
  `be_poor(m3)` + `z_after_delete` for every cursor with `pg[0] == mp`
  (`tree-ops.c`, empty-root branch) — including the root-collapse stack
  shift with `whole = top + stash` coverage (post-#454 hardening);
- `cursor_step` poor handling (`cursor-nav.c:161-171`): fresh → auto
  first/last; otherwise `z_poor_mark` + `(state & z_after_delete) ?
  MDBX_NOTFOUND : MDBX_ENODATA` — the first relative move consumes the
  after-delete marker (PREV → NOTFOUND), later relative moves → ENODATA;
- `cursor_brim` on an empty tree: `tree_search` returns MDBX_NOTFOUND for
  `root == P_INVALID` (`tree-search.c:46-50`) **before** any `pg[top]`
  dereference — no stale-page read;
- `GET_CURRENT`: `is_hollow` → `MDBX_ENODATA`;
- R2: A re-seeks and re-puts normally after the collapse.
Observed results asserted: GET_CURRENT/NEXT/PREV_NODUP → ENODATA after the
marker is consumed; PREV → NOTFOUND; FIRST/LAST → NOTFOUND. Clean under the
live-assert build (cursor_validate, txn_dpl_check all hold).

---

## 3. Attacks tried → held (no repro needed; held-reasons)

### cursor_put / convert_to_subtree / sub-page growth (attacks 2, 3, 11) — HELD

- **uint16 truncation of `fp->dupfix_ksize = (uint16_t)data->iov_len`
  (`cursor-mut.c:255/431`)**: unreachable through the API. Chain:
  `valsize_max(DUPFIXED) = keysize_max(ps, 0) = BRANCH_NODE_MAX - NODESIZE`
  (`cogs.h:110-120`) < `leaf_nodemax` < `UINT16_MAX` (STATIC_ASSERT +
  `ENSURE` at `env.c:49-50`); `cursor_put_checklen` enforces
  `data->iov_len <= clc->v.lmax`; a crafted `db->dupfix_size` is rejected at
  cursor setup (`table.c:30-35`, `cursor-setup.c` mismatch branch). INV-C2.
- **`mp->upper = fp->upper + growth` uint16 overflow / auxbuf overrun
  (`cursor-mut.c:596-597`)**: `xdata.iov_len <= env->subpage_limit` or the
  `node_size_len(...) > leaf_nodemax` gate (`cursor-mut.c:556`) forces
  convert_to_subtree; `subpage_limit = (leaf_nodemax - NODESIZE) * option(<=
  65536) >> 16 <= leaf_nodemax - NODESIZE` (`page-ops.c:770-775`, option
  clamped to 65535 at `api-opts.c:439-450`); `leaf_nodemax =
  EVEN_FLOOR(PAGESPACE/2) - sizeof(indx_t)` ≈ ps/2. So
  `mp->upper <= leaf_nodemax - PAGEHDRSZ ≈ ps/2 - 24 << 65535` for every
  pagesize; the `cASSERT(fp->upper + growth < UINT16_MAX)` can never fire in
  release because the arithmetic cannot reach the bound. For the
  convert_to_subtree path `growth = ps - old_data.iov_len` with
  `subpage_geometry_ok` (validated `fp->upper <= old_len - PAGEHDRSZ`) →
  `mp->upper <= ps - PAGEHDRSZ`. The fake `fp` lives in `env->page_auxbuf`
  (ps-sized) and `xdata.iov_len <= leaf_nodemax < ps - PAGEHDRSZ` never
  overruns it. INV-C1.
- **crafted `page_numkeys(fp) == 0` / dupfix_ksize == 0**:
  `page_numkeys = mp->lower >> 1` (`cogs.h:300`) — no division by
  `dupfix_ksize`, no SIGFPE; the fake fp (lower=upper=0) yields a 0-item
  nested tree whose inner put is handled by the explicit `nkeys == 0` guard
  (`cursor-seek.c:25-34`, "при создании первой листовой страницы").
- **odd dupfix sizes**: parity tracked by the `(ksize & numkeys) ^ upper`
  invariant (`node.c:16`, asserted at add/after); memcpy lengths remain
  page-bounded.
- **APPEND with z_fresh on an empty dupsort tree (attack 11)**: `items == 0`
  → either the NO_ROOT path (height 0) or the seek's 0-numkeys guard; a
  0-item hive cannot persist (emptying the hive deletes the whole key in
  `cursor_del`; the empty-root rebalance keeps empty subpages only inside
  this transient window). Fresh auto-position then behaves exactly as
  append (ki=top set past the last row by the `cmp > 0` branch or insert at
  0).

### cursor_del / tree_drop (attack 4) — HELD

- **Partial tree_drop failure**: pages are retired exactly once (single
  walk, `page_retire[_ex]` per node/child; recursion depth bounded by
  CURSOR_STACK_SIZE + `tree_cutoff_twig`'s explicit deep check). A failure
  exits via `bailout` → `be_poor` → `cursor_del` fail →
  `mc->txn->flags |= MDBX_TXN_ERROR` (`cursor-mut.c:1171`). Containment:
  `cursor_check_rw`/`cursor_check_ro` pass `MDBX_TXN_BLOCKED ⊇
  MDBX_TXN_ERROR` as bad-bits and `check_txn` rejects
  `txn->flags & bad_bits` (`cogs.h:445-455`, `mdbx.h:1665`) — **every**
  subsequent cursor op on the poisoned txn returns MDBX_BAD_TXN, so other
  cursors' stale subcur (their hive pages are being retired) is
  unreachable. Abort discards the retired list with the txn. INV-C3.
- **N_TREE subtree drop + other cursors' subcur**: after a successful
  `tree_drop`, the `is_related_include(mc, m2) && pg[top]==mp && ki==ki`
  loop (`cursor-mut.c:1038-1041`) resets every related couple's inner via
  `inner_gone_unconditional` — including cursors deeper than mc
  (`is_related_include` allows `m2->top >= mc->top`). The `mc->tree->items`
  accounting for ALLDUPS subtracts `hive-1` first and the final `-1` at
  `del_key` — net `items - hive` exactly.

### cursor_del post-rebalance other-cursor fixup (attack 5) — HELD (structural)

`cursor-mut.c:1099-1161`: `top`/`mp`/`ki` are captured **after**
`tree_rebalance` (line 1106); rebalance's own fixups (empty-root collapse
`be_poor+z_after_delete`; root-collapse stack shift with
`whole = m3->top + m3->stash` coverage, `top -= 1`, `be_poor` when < 0;
page_merge's internal adjustments) keep related cursors consistent before
the loop; the loop gates on `top > m3->top || m3->pg[top] != mp` (stale
entries skipped), `m3->ki[top] >= nkeys` → `cursor_sibling_right(m3)` with
NOTFOUND → `inner_gone(m3)` + SUCCESS (deterministic eof marking by
`cursor_sibling_right`'s NOTFOUND path, `z_inner_tail_deleted` preserved by
the mask). Deeper collapse under m3 cannot desynchronize `m3->pg[top]`
against `mp` because every merge/rebalance step fixes related cursors at
that step (wave-1 pairs verified the enroot/undo pairs; the fixup loops
here are the same predicate family). No counterexample constructed;
defense-in-depth note: `m3->top == top` is forced by tree geometry (leaf
level is the deepest), so `m3->pg[top]` never indexes out of a shorter
stack.

### tree_search / comparator dispatch (attack 7) — HELD-by-design, documented

Sortedness of branch separators vs subtree keys is an **unchecked on-disk
invariant in release** (checked only by `mdbx_chk` / `MDBX_VALIDATION`
page_check). A crafted out-of-order branch page yields wrong-but-bounded
results: `search_branch` indices stay in `[0, nkeys-1]`, `page_get`
validates pgnos, `search_foliage` stays within the page — no OOB, no
signal. Same class: crafted `node_ks`/leaf2 `dupfix_ksize`·`numkeys`
product can push `page_dupfix_ptr` offsets beyond the page in release
(real leaf2 *pages* lack the release-mode geometry check that sub-pages
have via `subpage_geometry_ok` — #57/#176 hardened only the sub-page
path). Consistent with the documented corruption threat model (chk
required), but the leaf2-page geometry gap is worth a hardening note
(doc §5 below). No wrong-leaf-without-signals counterexample was
constructed for in-order pages, by definition of the comparator dispatch:
`cursor_to_search_foliage/branch` choose the comparator by
`clc->k.cmp` identity and the page's first key size — every dispatch
target implements the same order.

### cursor_seek states (attacks around V-029/032/033) — held except V-037

- The `ki == nkeys` fast-path encoding is consistently guarded at every
  consumer in these files: `GET_CURRENT` (`cursor-seek.c:287`),
  `cursor_step` (eof handling), `del` (`!is_filled` → advance/no-op),
  `cursor_seek` re-entry (re-seek normalizes), `cursor_forward_at`
  (step==0 forces sibling_right), `TO_PAIR_EQUAL` normalization
  (`cursor-seek.c:620-630` rewrites to the uniform past-the-tail state).
  The only unguarded consumers are the rank APIs → V-037.
- `TO_KEY_*`/`TO_PAIR_*` ops pass `MDBX_SET_RANGE` to `cursor_seek`, so
  the `z_hollow` `target_not_found` state is unreachable for them; the
  LESSER/GREATER settle-on-neighbor semantics (§5 R4) hold through
  `outer_prev/outer_next` from both eof encodings.
- `SET_UPPERBOUND`/`SET_LOWERBOUND` RESULT_TRUE mapping verified
  (`cursor-seek.c:454-495`); past-end LOWERBOUND returns MDBX_NOTFOUND
  (not RESULT_TRUE) with the cursor left pointed+eof — this is the state
  V-037 consumes.

### cursor_distance/distribute (attacks 9, 10) — held (release), V-037/V-039 recorded

- `cursor_distribute`: begin/end must be pointed (`api-cursor.c:1013/1021`);
  `tree_diff_level` INT_MIN → ENODATA; `begin == end` → MDBX_RESULT_TRUE;
  `amount < array_size` → excess cursors `be_poor` + RESULT_TRUE
  (zero-fill semantics); the scratch `iter` must be a non-begin/end array
  element (EINVAL otherwise); on `cursor_distance` error the scratch iter
  is left in a walked state (unspecified-on-error, begin/end untouched) —
  doc note.
- `cursor_distance_at` wrap → absorbed in distribute (V-035), leaks in
  `mdbx_cursor_distance` (V-037), acknowledged gap for multi-page hives
  (V-039).
- `cursor_diff` (`api-range-estimate.c`) independently re-implements rank
  arithmetic with `depth < 0 → ENODATA` and page-identity verification
  (`MDBX_PROBLEM` on mismatch) — the poor-vs-pointed top mismatch is
  caught there (unlike `tree_diff_level`'s checking-only ASSERT);
  `mdbx_cursor_distribute`/`mdbx_cursor_distance` reject poor cursors at
  the API gate.

### cursor_put_checklen / INTEGERKEY pinning (attack 12, P6) — HELD-by-contract, V-040 doc

Mixed 4/8-byte INTEGERKEY is rejected with MDBX_BAD_VALSIZE after the first
put pins `clc->k.lmin = clc->k.lmax = key->iov_len`
(`cursor-mut.c:887-888`). This enforces the documented contract
("The keys must all be of the same size", mdbx.h:1686) and is load-bearing
for the `cmp_uint_align4` dispatch (4-byte keys are 4-aligned only because
all keys share the size; `tree-search.c:568` "alignment is guaranteed").
Residuals (V-040):
1. the pin is applied **before** `cursor_put`, so a FAILED first put
   (e.g. NOOVERWRITE/KEYEXIST, ENOMEM) still pins the comparator size for
   the rest of the env's lifetime;
2. `clc` lives in `env->kvs[dbi]` (env-global; `couple_init`:
   `couple->outer.clc = &kvx->clc`, initialized once by
   `tbl_setup_ifneed` when `v.lmax == 0`) — the pin persists across
   transactions, and reader/writer accesses to `lmin/lmax` are formally
   racy (aligned 8-byte loads on mainstream hardware; semantically
   consistent because the pin always matches the tree contents;
   `cursor_to_search_foliage` re-pins from `dupfix_size`/node keys at
   `tree-search.c:433-436/474`, `cursor_dupsort_setup` re-pins from
   `nested_tree.dupfix_size` at `cursor-setup.c:435`). No corruption
   construct; documented as ownership/contract behavior. INV-C4.

### cursor-nav long-range helpers — HELD

- `cursor_forward_at`: `step == 0` (past-end fast encoding) forces the
  sibling_right; `ki += distance` only under `ASSERT(distance < step)`
  (release: distance < step holds by construction after the loop);
  eof_soft at the last row: step == 1 → sibling_right lands exactly on the
  next page's first row (rank-correct).
- `cursor_backward_at`: the wrapped `ki` write before `sibling_left` is
  overwritten on both NOTFOUND (ki reset to 0 by
  `cursor_sibling_left`) and SUCCESS paths; underflow of `distance`
  is bounded by sibling_left's NOTFOUND exit.
- `cursor_scroll_forward/backward`: `tree_deepen_edge` at leaf level is a
  no-op SUCCESS (the `--amount == 0` landings); the inner
  `cursor_forward_at(amount-1, level - height)` keeps level arithmetic
  non-negative (`level >= height` is the scroll-path precondition);
  `cursor_dupsort_setup` failures bubble as errors with the cursor left
  consistent (inner_gone before re-setup).
- `sibling()`: root-no-siblings NOTFOUND before any page access;
  `bailout` asserts `err != MDBX_NOTFOUND` (NOTFOUND is returned only by
  the deliberate unwinding paths, which restore the enrooted stack via
  `cursor_undo_enroot`).

---

## 4. New invariants (for libmdbx-invariants.md)

- **INV-C1 (sub-page growth bound).** `leaf_nodemax =
  EVEN_FLOOR(PAGESPACE/2) - sizeof(indx_t) < UINT16_MAX` (STATIC_ASSERT +
  ENSURE, `env.c:44-50`); `env->subpage_limit <= leaf_nodemax - NODESIZE`
  regardless of user options (`recalculate_subpage_thresholds`,
  option clamped to ≤ 65535); every sub-page growth path caps
  `xdata.iov_len` by `subpage_limit` or the
  `node_size_len > leaf_nodemax` convert gate (`cursor-mut.c:478-480/556`);
  `page_subleaf2_reserve` caps the reserve by `subpage_limit` itself.
  Therefore `mp->upper = fp->upper + growth < UINT16_MAX` cannot overflow
  in release, and the fake `fp` never overruns `env->page_auxbuf`.
- **INV-C2 (dupfix size bound).** For MDBX_DUPFIXED trees
  `valsize_max = keysize_max(ps, 0) = BRANCH_NODE_MAX - NODESIZE <
  leaf_nodemax`; enforced by `cursor_put_checklen` against `clc->v.lmax`
  and by table/cursor setup against `db->dupfix_size`. Hence
  `(uint16_t)data->iov_len` casts at `cursor-mut.c:255/431` never truncate,
  and dupfix elements always fit a node.
- **INV-C3 (poisoned-txn cursor containment).** Any cursor mutation path
  that fails after a partially-applied state sets
  `MDBX_TXN_ERROR` (`cursor-mut.c:839/1171` and the `fail:` labels);
  `cursor_check[_rw/_ro/_pure]` pass `MDBX_TXN_BLOCKED ⊇ MDBX_TXN_ERROR`
  to `check_txn`, which rejects `txn->flags & bad_bits` — so after a
  failed del/put/put-MULTIPLE every subsequent cursor operation (read or
  write, any cursor of the txn) returns MDBX_BAD_TXN and no stale
  pg[]/subcur state is reachable. Load-bearing for tree_drop partial
  failure and mid-batch put failure.
- **INV-C4 (clc ownership and pin discipline).** `mc->clc` points into
  `env->kvs[dbi]` (env-global, initialized once by `tbl_setup_ifneed`);
  the INTEGERKEY/INTEGERDUP/DUPFIXED size pins
  (`cursor_put_checklen`, `cursor_to_search_foliage`,
  `cursor_dupsort_setup`) are env-lifetime sticky, cross-txn, and
  formally racy reader/writer on aligned size_t — semantically consistent
  because the pin always mirrors the tree's actual uniform element size.
  The pin is the enforcement mechanism of the documented
  "keys/values all of the same size" contract.
- **INV-C5 (past-end rank dual-encoding disease, unified statement).**
  `SET_RANGE` past-end leaves either `ki = nkeys` (fast path) or
  `ki = nkeys-1` + eof flags (sibling failure). Every consumer that reads
  the position literally (GET_CURRENT family) diverges (V-029/032); every
  consumer that derives rank from it must choose one convention —
  `cursor_distribute` absorbs, `cursor_diff`/`cursor_distance_at` leak
  (V-037). Uniformity requires a single canonical encoding or a single
  rank formula; until then the two encodings are observable through
  public APIs.
- **INV-C6 (full-collapse navigation).** Empty-root collapse
  (`tree_rebalance`) applies `be_poor` + `z_after_delete` to every cursor
  whose `pg[0]` is the emptied root; `cursor_step` poor handling
  distinguishes fresh (auto-position), after-delete (first relative move
  NOTFOUND, then ENODATA), and plain-poor (ENODATA); `cursor_brim`'s
  `tree_search` NOTFOUND for `root == P_INVALID` precedes any `pg[top]`
  dereference. After a full collapse all follow-ups are deterministic
  NOTFOUND/ENODATA and the cursor re-seeks normally (machine-checked by
  `repro/v5b-empty-tree-nav.c`, incl. height-2 merge chains).

## 5. §5 outcomes-table amendments (libmdbx-invariants.md)

Add to the table's notes (the table rows themselves stay; these are the
documented deviations/edges discovered):

1. **SET_RANGE/UPPERBOUND/LOWERBOUND past-end** — the row's state
   "z_eof_soft на последней строке" holds for the sibling-failure path;
   the fast path leaves `ki = nkeys` (no readable row). GET_CURRENT
   diverges accordingly (V-029). **Additionally (V-037):** the derived
   rank/estimate results diverge (-2/-1/0) across
   `mdbx_estimate_distance` / `mdbx_cursor_distance(deepness)` /
   `mdbx_estimate_range` for the same logical range. R1 is violated at
   the estimate layer, not at the CRUD layer.
2. **SET_LOWERBOUND past-end returns MDBX_NOTFOUND** (not
   MDBX_RESULT_TRUE) — the cursor stays pointed with eof flags; legal to
   continue from, but the §5 wording "даёт позицию первого большего либо
   строгое «за концом»" should name the exact return codes per
   sub-case.
3. **MULTIPLE|NODUPDATA mid-batch duplicate returns MDBX_KEYEXIST
   uniformly** (inline hive and sub-page hive alike); done counter =
   elements before the duplicate; the prefix belongs to the txn (commit
   persists done-count elements; abort discards). The
   `dupsort_error → MDBX_PROBLEM` re-mapping is reachable only for batch
   element 0 (insert_key) failures — worth a line in the module
   interfaces doc so the "Unexpected KEYEXIST" ERROR message is not
   mistaken for a live hazard.
4. **mdbx_cursor_distance/deepness over multi-page nested trees** is
   documented in-code as approximate (rank discount skipped for
   `nested_tree.height > 1`, V-039); the estimate-vs-hive contract
   should state the limitation or the fix should land.

## 6. test-coverage.md gaps

1. No test drives `mdbx_cursor_distance` / `mdbx_estimate_distance` with
   a cursor parked past-end by `SET_LOWERBOUND` (the V-037 corpus).
   Suggested: golden matrix {past-end encoding} × {api, deepness} ×
   {height 1..3} asserting ONE canonical answer each.
2. No test covers `MDBX_MULTIPLE|MDBX_NODUPDATA` with in-vector
   duplicates at boundary positions (element 1 vs element k) — the
   uniform-KEYEXIST semantics (V-038-not-a-bug) and the done/base
   accounting deserve regression pins (repro E1-E3 are ready to be
   promoted into `tests/ut`).
3. No test navigates cursors through a **full-tree collapse performed by
   another cursor** across height-2 merge chains (repro
   v5b-empty-tree-nav) — the be_poor+z_after_delete fixup and the
   first-relative-move-NOTFOUND convention are unpinned.
4. No checking-build assert covers the `cursor_distance_at`/`cursor_diff`
   rank paths (V-037 is silent under MDBX_DEBUG asserts); an
   `ASSERT(distance sanity)` or a canonical-rank helper would have caught
   it.
5. Release-mode geometry validation exists for sub-pages
   (`subpage_geometry_ok`, #57/#176) but **not** for real leaf2 pages
   (`dupfix_ksize * page_numkeys` vs page space) — mdbx_chk-only today;
   a cheap O(1) release check on first touch would close the crafted-page
   read-overrun class noted under attack 7.
6. The clc size-pin behavior (V-040): no test pins the pin (FAILED first
   put still pins; BAD_VALSIZE for the other size afterwards). If the
   owner judges the pin-before-failure ordering undesirable, the fix is
   to pin only after a successful insert; otherwise document.

## 7. Module-interface contract additions (for module-interfaces.md)

- `cursor_seek(mc, key, data, MDBX_SET_RANGE)` — pre: cursor_check_ro,
  key within [k.lmin, k.lmax or unpinned]; post-success: filled + inner
  positioned (dupsort); post-NOTFOUND: either filled-on-neighbor (exact
  miss, op < SET_RANGE → hollow) or pointed+eof (past-end, dual encoding
  INV-C5); post-error: poor.
- `cursor_put(..., MDBX_MULTIPLE)` — pre: DUPFIXED db, data[0] =
  {elements, element-size}, data[1].iov_len = count; post:
  data[1].iov_len = number of elements stored (duplicates counted as
  stored-no-op without NODUPDATA; batch stops at the first NODUPDATA
  duplicate with MDBX_KEYEXIST), data[0].iov_base advanced past the
  consumed elements, may be resumed; the inserted prefix is transactional.
  A failure of `cursor_touch` between elements returns the touch error
  with the same accounting.
- `cursor_del(mc, flags)` — pre: filled, or inner tail-deleted (no-op
  SUCCESS), or eof/outer-advance per the predicate block; post: the
  cursor on the next element (`z_after_delete`), or poor when the tree
  emptied; every related cursor's inner re-anchored or gone; failure ⇒
  INV-C3 containment.
- `cursor_distance/distribute` — pre (API): both cursors pointed, same
  dbi, scratch array element distinct from begin/end; post: cursors
  distributed at `level` granularity, excess → poor + MDBX_RESULT_TRUE;
  error ⇒ begin/end preserved, scratch state unspecified.
