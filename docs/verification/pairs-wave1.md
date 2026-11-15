# Pairs verification report — wave 1 (stream C, structure→consumer)

All files under /home/doom/Projects/libmdbx-audit/src/. Read-only audit.

---

## Pair 1: pnl.c → gc-get.c / gc-put.c

**Primitive contracts** (pnl.c/h):
- `pnl_merge(dst, src)` (pnl.c:230): pre — `pnl_check_allocated(dst)`, `pnl_check_lite(src)`, `alloclen(dst) ≥ dst_len + src_len`; post — returns new `pnl_size(dst)` = `sum` (disjoint-range paths or no dup seen) or `sum − 1` iff an equal pair was seen between the two lists (flag, not counter, pnl.c:194–222, gated by `!MDBX_DISABLE_VALIDATION`, options.h:133 → 0 by default). Index 0 is never written (P_INVALID sentinel at pnl.c:248 stops `src_a` draining); output is weakly descending; **when the dup flag fires the visible content is garbage** (trace: dst=[5,2]+src=[2,1] → visible [5,2,2], tail 1 lost) — the deficit return is the only reliable signal and the list must be discarded (invariant PAIR-N1).
- `pnl_need` (pnl.h:97): post — `alloclen ≥ size + more` or error.
- `pnl_setsize` (pnl.h:53): pre — `len < INT_MAX`.
- `MDBX_PNL_MOST/EDGE/FIRST/LAST`: descending mode; EDGE = `&pnl[len]`, MOST = `pnl[1]` (largest).

### Enumeration (gc-get.c)
| site | use | precondition check | verdict |
|---|---|---|---|
| gc-get.c:1174 | `pnl_need(&repnl, gc_len)` before merge | covered by PAGELIST_LIMIT guard in pnl_reserve; err propagated (1175) | OK |
| gc-get.c:1188–1189 | `expected_after_merge = gc_len + pnl_size(repnl)`; `has_duplicates = pnl_merge(...) != expected` | alloclen reserved by 1174; src pre-checked by `gc_check_rowdata` (923–938: lite by default, full under MDBX_VALIDATION) satisfying `pnl_check_lite(src, MAX_PAGENO+1)`; dst descending invariant maintained | OK |
| gc-get.c:1223–1227 | deficit → `MDBX_CORRUPTED`, `goto fail` | fail path (1476–1493) sets MDBX_TXN_ERROR for non-reserve/UNIMPORTANT; reserve paths propagate → txn fails | OK |
| gc-get.c:1240, 1479 | `MDBX_PNL_MOST(repnl) == first_unallocated-1` → `txn_refund` | MOST = largest ✓; refund.c:10 asserts the same | OK |
| gc-get.c:1146–1152, 1299–1306 | `repnl_get_single` / `gc_repnl_get_sequence` | guarded by `pnl_size(repnl) >= num` (1143, 1296) | OK |
| gc-get.c:577, 631–654, 656–667 | `MDBX_PNL_EDGE` + dir arithmetic in `repnl_get_single` | len>0 pre (672 guard; 576 assert); SAVING_SEQUENCES branch dead by default (628–630) and in-bounds for len>2 | OK |
| gc-get.c:682–683, 727–731, 761–768 | `edge[-seq]`/`edge[-num]` reads, `pnl_setsize(len-num)` | `len ≥ num` asserted; `edge[-num]` read guarded by `len == num ||` short-circuit | OK |
| gc-get.c:993–995, 1144–1145, 1297–1298 | `MDBX_PNL_LAST/FIRST < first_unallocated` asserts | bounds maintained by merge/checks | OK |

### Enumeration (gc-put.c)
| site | use | precondition check | verdict |
|---|---|---|---|
| gc-put.c:297, 301 | `pnl_need(&retired_pages, loose_count)` + `pnl_append_prereserved` per loose page | reserve first ✓; retired_pages unsorted during the txn (also page-ops.c:177), re-sorted before GC-record write at gc-put.c:382/461 | OK |
| gc-put.c:308 | `pnl_need(&repnl, 2*loose_count + 2)` | err propagated | OK |
| gc-put.c:311–321 | temp `loose` list at top of repnl allocation, `pnl_setsize`, `pnl_sort` | computed **after** pnl_need (no stale pointer after realloc); temp zone `[alloclen−c−1, alloclen−1]`; count>0 asserted | OK |
| gc-put.c:322 | `pnl_merge(repnl, loose)` — **return ignored** | alloclen ≥ size+2c+2 ⟹ merge write range [1, size+c] disjoint from temp zone [size+c+2, alloclen−1] (the `+2` slack is exactly the overlap-freedom margin); both inputs descending | OK — but see PAIR-F1 |
| gc-put.c:553–556 | `MDBX_PNL_CONTIGUOUS` scan of repnl | repnl sorted maintained | OK |
| gc-put.c:382, 461 | `pnl_sort(retired_pages, first_unallocated)` | entries < first_unallocated (page-ops gates) | OK |

**Verdict: CONSISTENT·A** (assumptions PAIR-F1–F3).

---

## Pair 2: dpl.c → txn-basal.c `txn_write()` + spill.c

**Primitive contracts** (dpl.h/dpl.c):
- `txn_dpl_sort` (dpl.h:32): post — `items[1..sorted]` ordered by pgno, `sorted == length` on return.
- `txn_dpl_search` (dpl.c:154): handles unsorted tail (linear ≤7, else full sort); returns 1-based first-≥ position — caller must re-verify equality.
- `dpl_setlen` (dpl.h:8): sets `length`, re-arms the `items[len+1] = P_INVALID` sentinel; returns len (idiom `dl->sorted = dpl_setlen(dl, w)`).
- `dpl_npages/dpl_endpgno`: respect `MDBX_DPL_CACHE_NPAGES` cache.
- `pages_including_loose` invariant: equals Σ npages over all entries (txn_dpl_check dpl.c:411).

### Enumeration — txn-basal.c `txn_write()` (lines 6–64)
| site | use | verdict |
|---|---|---|
| txn-basal.c:8 | `txn_dpl_sort` at entry → full sort, `sorted == length` | OK |
| txn-basal.c:11–36 | iterate sorted list; loose kept (`items[++w] = items[r]`), non-loose written+dropped; no dpl search during iteration (list untouched until tail loop) | OK |
| txn-basal.c:17–19 | `dpl_npages(dl, r)` per written page; `total_npages` includes the later-failed page | OK |
| txn-basal.c:21–34 | failure paths: non-WRITEMAP releases failed shadow (28), drops it via `++r` (29); WRITEMAP early-returns with list untouched | OK |
| txn-basal.c:49 | `pages_including_loose -= total_npages` — subtracts exactly the npages of removed (written + failed) entries | OK |
| txn-basal.c:50–54 | tail compaction (subsequence of sorted → still sorted); `dl->sorted = dpl_setlen(dl, w)`; `dirtyroom += r−1−w` | OK — all three invariants (sorted-prefix, sentinel, dirtyroom+length==limit) restored |
| txn-basal.c:60–61 | post asserts `length == loose_count`, `pages_including_loose == loose_count` | holds because loose pages always have npages==1 (page-ops.c:598 gate) — invariant PAIR-N3 |

### Enumeration — spill.c
| site | use | verdict |
|---|---|---|
| spill.c:90–99 (`spill_cursor_keep`) | `txn_dpl_search` + equality re-check + `txn_dpl_age(n) > 0` guard | OK — correct use of search post-condition; called after txn_dpl_sort (268) |
| spill.c:196–204 | `dirty_entries = length − loose_count`, `dirty_npages = pages_including_loose − loose_count` | OK (loose npages==1, PAIR-N3) |
| spill.c:224–226, 229 (WRITEMAP/AVOID_MSYNC) | `sorted = dpl_setlen(dl, 0)`, `pages_including_loose = loose_count`, re-append loose via `txn_dpl_append` | OK — append's insertion logic re-sorts (dpl.c:303–333), dirtyroom arithmetic consistent |
| spill.c:268 | `txn_dpl_sort` before spill scan | OK |
| spill.c:271 (`spill_txn_keep`) | `txn_dpl_lru_turn` once; then search on sorted list | OK |
| spill.c:309–334 | age scan + `dpl_npages` per entry | OK |
| spill.c:374–397 | spill loop: `items[++w] = items[++r]` compaction over the sorted array; backward adjacency scan `dpl_endpgno(dl,w) == items[w+1].pgno` (382) relies on sortedness — holds (copies of originals in order); spill order ascending-by-pgno → spilled PNL temporarily reversed, fixed by `pnl_sort` (421) | OK |
| spill.c:403–411 (`failed:`) | failed entry dropped from list but neither written nor spilled; `dirtyroom += spilled_entries`, `pages_including_loose -= spilled_npages` include it | OK-by-abort — `bailout` (254–256) sets MDBX_TXN_ERROR; accounting stays self-consistent; assert at 406 documents the mismatch window — see PAIR-F6 |
| spill.c:408 | `dl->sorted = dpl_setlen(dl, w)` | OK — remaining entries are a sorted subsequence |

Adjacent setlen sites checked for the same discipline: gc-put.c:347–349 (`sorted −= sorted_out` before `dpl_setlen(dl, w)` — correct), refund.c:101–116/124–138 (prefix loop keeps `sorted = w`; tail path converts surplus sorted into refunded — correct), dpl.c:498 (`txn_dpl_sift`), txn-nested.c:21/308/318.

**Verdict: CONSISTENT·A** (relies on invariants PAIR-N2/N3; PAIR-F6 noted).

---

## Pair 3: cursor.h predicates → cursor-mut.c

**Primitive contracts** (cursor.h): zone gates Z0 `flags<0x08` / Z1 `0x08..0x3F` (edge, still filled) / Z2 `0x40..0x7F` (eof, CRUD forbidden) / Z3 `<0` (hollow/poor); `is_filled` = `(uint8_t)flags < z_eof_hard` (210); `is_pointed` = `top ≥ 0 == combo_state ≥ 0` (187); `inner_tail_deleted` = exactly `z_inner_tail_deleted` among `{z_hollow, z_after_delete, z_inner_tail_deleted}` (258); `eof_hard_clear` clears z_eof_hard only when pointed at an element and not tail-deleted (239–242). **Critical: `F_ISSET(w,f)` is ALL-bits (`utils.h:7`).**

### Enumeration — `cursor_del()` (cursor-mut.c:924–1173)
| site | use | verdict |
|---|---|---|
| cursor-mut.c:925–930 | early `!is_filled(mc)` + `F_ISSET(flags, z_inner\|z_eof_hard)` → `outer_next(cursor_outer(mc), NEXT)` | OK — **cannot violate the outer cursor's contract**: F_ISSET is all-bits, so any *outer* cursor (poor 0xFFC4, fresh 0xFFC4, Z2-eof 0x40.., after-delete+eof 0x68) lacks `z_inner` ⟹ `F_ISSET` false ⟹ MDBX_ENODATA, exactly per "no CRUD in Z2/Z3"; the advance branch requires `z_inner`+`z_eof_hard` simultaneously ⟹ inner cursor only, and `cursor_outer(mc)` is then valid. Moreover the only inner entry (`cursor_del(&mc->subcur->cursor, 0)` at 966) is pre-guarded by `is_filled` (958) ⟹ the advance is currently unreachable defense-in-depth |
| cursor-mut.c:958–964 | `!is_filled(inner)` → `inner_tail_deleted(...) ? MDBX_SUCCESS : MDBX_ENODATA` | OK — mask precisely selects the tail state: poor inner (0xFFC1) → ENODATA; tail 0x71 → no-op ✓; after-delete-eof 0x78 → ENODATA ✓ (matches "no-op must not fire for advanced position") |
| cursor-mut.c:991, 780 | beyond-tail other-cursors: `flags \|= z_eof_hard\|z_eof_soft\|z_after_delete` + `inner_gone_unconditional` | OK — synthesizes Z2 so CRUD stays blocked; `eof_hard_clear` will refuse to clear it since `ki == page_numkeys` |
| cursor-mut.c:1003–1004 | inner pointed with `ki ≥ numkeys` → mark `z_inner\|z_eof_soft\|z_eof_hard\|z_inner_tail_deleted` | OK |
| cursor-mut.c:1021–1022 | `!is_filled(mi)` → `\|= z_inner_tail_deleted` | OK — poor+tbit result (0xFFD1) still fails `inner_tail_deleted`'s mask (z_hollow set) so the no-op cannot mask a real value |
| cursor-mut.c:1045 | `cASSERT0(!inner_pointed(mc))` on non-N_DUP node | OK |
| cursor-mut.c:1067–1079 | adjust others: `ki==deleted` → `z_after_delete`+`inner_gone` (stays Z1-filled ✓); else decrement; `inner_pointed` → `cursor_inner_refresh` | OK |
| cursor-mut.c:1109 | `top > m3->top \|\| m3->pg[top] != mp → skip` — poor m3 (`top = −1`) skipped via sign trick | OK |
| cursor-mut.c:1122–1131 | `!m3->subcur → continue`; `!is_filled(m3) → inner_gone_unconditional(m3)` | OK — safe only because an **inner cursor always has `subcur == nullptr`** (cursor-setup.c:311/501), so m3 here is guaranteed to be an outer; this gate is load-bearing (invariant PAIR-N4) |
| cursor-mut.c:1144–1158 | `is_pointed(&m3->subcur->cursor)` → re-anchor pg[0]; else `cursor_dupsort_setup`(+`inner_first`)+`z_after_delete` | OK |

### Other predicate sites in cursor-mut.c
| site | use | verdict |
|---|---|---|
| cursor-mut.c:19 | `cASSERT0(mc->stash < 1)` entry gate for cursor_put | OK |
| cursor-mut.c:49 | `cASSERT0(inner_pointed(mc))` under N_DUP+CURRENT | OK |
| cursor-mut.c:105 | APPEND fast-end: `is_pointed(mc) && !(flags & (z_inner\|z_hollow))` — reads pg/ki stack; the ki-loop (107–110) independently verifies the right edge, so Z1 edge states (after_delete/eof_soft, all `< 0x40` ⟹ filled) are safe | OK |
| cursor-mut.c:90, 186, 287 | `is_poor` gate for NO_ROOT; `be_poor` on seek error / CURSOR_FULL | OK |
| cursor-mut.c:166, 274–292, 686–687, 803, 808 | `inner_pointed`; fix_parent enroot/undo (pair-4 idiom); `be_filled` after insert | OK |
| cursor-mut.c:172–173 | after ALLDUPS del: `mc->top+1 == tree->height`; `top ≥ 0 ? NOTFOUND : NO_ROOT` | OK |

### `eof_hard_clear` call sites (not used in cursor-mut.c — all 3 pairs verified)
cursor-seek.c:422–423 (after NEXT ops; comment documents why stale z_eof_hard must not leak into rebalance-after-delete), api-cursor.c:840–841 and 902–903 (bunch-delete loops before re-`cursor_del`). Each applies the point-of-element guard to **both** outer and inner; past-tail positions keep z_eof_hard so CRUD stays blocked. OK.

**Verdict: CONSISTENT** (no violations; the initially-suspected 924–930 hazard is refuted by F_ISSET all-bits semantics — invariant PAIR-N5).

---

## Pair 4: cursor_push/eject/enroot/undo_enroot → cursor-nav.c / cursor-setup.c

**Primitive contracts** (cursor.h:386–426): `cursor_push` pre `top ≥ −1`, bound `new_top < CURSOR_STACK_SIZE` (else be_poor+CURSOR_FULL), post `stash = 0` (16-bit write resets stash); `cursor_eject` pre `top ≥ 0 ∧ stash == 0`; `cursor_enroot(step)` pre `top ≥ step ∧ stash ≥ 0`, post `top = prev−step ≥ 0, stash = prev+step > 0`; `cursor_undo_enroot(step)` post `top = prev+step > 0, stash = prev−step ≥ 0`. Packed: `top_and_stash = (int8 top) | (int8 stash << 8)` inside `combo_state` (internals.h:314–359); `CURSOR_STACK_SIZE = 16 + MDBX_WORDBITS/4` (32 on 64-bit).

### Enumeration
| site | use | verdict |
|---|---|---|
| cursor-nav.c:11–49 (`sibling`) | pre `top ≥ 1`; enroot(1) at 17; NOTFOUND → undo(1) at 26 (post `top = initial ≥ 1 > 0` ✓, stash 0 ✓); other errors → `be_poor` (48) which forces `top = stash = −1` — subsequent `cursor_push` re-legitimizes (pre `top ≥ −1` ✓, single write resets stash to 0) | OK |
| cursor-nav.c:41 | `cursor_push(mc, mp, ...)` after enroot: with stash>0 the push overwrites the stashed child slot pg[top+1] — intended (sibling replaces the old child) and the push bound also caps `top+stash` | OK |
| cursor-nav.c:348–349, 387–388 (`cursor_forward/backward_at`) | raw `iter->top_and_stash = (uint16_t)level` — bulk truncate to `level`, discards stash | OK — ASSUMPTION stash==0 (settled iteration cursors) and `level ≥ 0`: both guaranteed — callers handle `level < tree->height` before inner-cursor recursion (425/431, 469/475), api-cursor.c:1063/1075/1077/1152 clamps `level < CURSOR_STACK_SIZE`; cursor-seek.c:670–673 same idiom, inner call at cursor-seek.c:759 gets `level − tree->height ≥ 0` from the branch at 733 — see PAIR-F4 |
| cursor-setup.c:185–195 (`cursor_touch`) | manual enroot loop `i = top<<8; … top_and_stash = i; page_touch; i += 1−(1<<8)` — walks levels 0..top, each touched once; final ASSERT `top ≥ 0 ∧ top < CURSOR_STACK_SIZE ∧ stash == 0` | OK — pre `stash < 1` asserted at 138; no pushes inside the loop; `top ≤ 31` guaranteed by push-bound so `top<<8` fits uint16 |
| cursor-mut.c:274–292 (`fix_parent`) | `do { cursor_enroot(mc,1); } while(top && !ki[top])`; then `if (stash ≥ shift)` + bound `top+stash+shift ≥ CURSOR_STACK_SIZE` → be_poor+CURSOR_FULL; `cursor_undo_enroot(mc, shift)` | OK — `stash == shift` invariant makes the guard always-true (defensive); undo post `top = original ≥ 1 > 0` ✓ |
| page-split.c:291/299, 335/353, 360/363, 390/394 | paired enroot/undo around parent updates & `tree_propagate_key`; failure paths skip undo deliberately (rc → txn error; cf. tree-ops.c:524 comment) | OK — undo posts hold: pre-enroot tops ≥ 1; `mn` inserted into `txn->cursors` (293–294) so the recursive split keeps its stack tracked |
| tree-ops.c:151/187, 348/381, 518/528, 564–590 | merge-path enroots; 570 enroot(cdst,1) + rebalance; leaf case `stash = 0` (583), non-leaf `top_and_stash = save_top − delta` (588) with assert `0 ≤ top < height < CURSOR_STACK_SIZE` (589) | OK — PAIR-F5: `delta ≤ save_top` is a structural rebalance invariant, assert-only |
| api-copy.c:160, 254–261, 284 (`compacting_walk`) | reset 0; manual push `top_and_stash = top+1` **before** the `top ≥ deep_limit` CURSOR_FULL check — but the bound check precedes any `pg[]/ki[]` write (259/261), so no OOB; `cursor_eject` at 284 with `mc->top != 0` guard (281) → post `top ≥ 0`, stash==0 maintained throughout | OK |
| tree-search.c:59, api-get-cached.c:172 | `top_and_stash = 0` fresh-descent resets (pg[0] re-fetched at 53–57) | OK |
| tree-cutoff.c:243, 302 | `top_and_stash = (uint16_t)level` on a cloned cursor (242 `cursor_cpstk`) / on related cursors gated by `m3->top < level → skip` (290) ⟹ `level ≤ m3->top` | OK |
| page-split.c:402–408 | manual `stash += 1` with explicit bound `top + stash ≥ CURSOR_STACK_SIZE → CURSOR_FULL` before `pg[top+stash] = mp` | OK |

### CURSOR_STACK_SIZE / tree-height bound chain (invariant PAIR-N6)
`meta_validate` rejects stored `height ≥ CURSOR_STACK_SIZE` for GC (meta.c:633) and Main (meta.c:669) with the explicit rationale "would walk the stacks out of bounds during tree_rebalance() root-collapse"; nested dup-trees rejected at cursor-setup.c:375. Hence max descent depth `height−1 ≤ 30` ⟹ `cursor_push`'s CURSOR_FULL guard is unreachable for validated trees — pure safety net; every raw `top_and_stash` write is bounded by the same chain.

**Verdict: CONSISTENT·A** (assumptions PAIR-F4 on stash==0 at raw-truncate sites; PAIR-F5 on the rebalance delta).

---

## Findings

| V-id | file:line | severity | description |
|---|---|---|---|
| PAIR-F1 (V-018) | gc-put.c:322 | low (assumption) | `pnl_merge(repnl, loose)` return ignored — the duplicates flag (sum−1 deficit) is unused, unlike the always-on check at gc-get.c:1188–1189. Relies on structural disjointness: loose pgnos are current-txn dirty pages consumed from repnl at allocation time, while repnl additions come only from prior-txn GC records (and this same function). If disjointness ever broke, a duplicate pgno would silently persist in repnl (content after a flagged merge is garbage — PAIR-N1) → double allocation. Cheap defense-in-depth (compare against `pnl_size(repnl)+count`) would match gc-get's discipline. Same pattern at txn-nested.c:324 (parent/nested spilled lists; disjointness maintained via spill_remove/spill_purge, page-ops.c:106–112, txn-nested.c:170). |
| PAIR-F2 (V-010) | pnl.c:198, 218 + gc-get.c:1189 | informational | The dup flag is gated by `!MDBX_DISABLE_VALIDATION`; with `MDBX_DISABLE_VALIDATION=1` the gc-get "real always-on check" (comment gc-get.c:1204) silently never fires (return always == sum). Default is 0 (options.h:133), so consistent by default. |
| PAIR-F3 (V-022) | pnl.c:191–222, gc-get.c:937 | informational (documented) | `pnl_merge` detects only *cross-list* equal pairs at the merge heads; duplicates *within* one GC record are caught only by `pnl_check_full` (MDBX_VALIDATION at gc-get.c:935–937, or CHECKS2 post-merge at 1230–1235). In a default release build a crafted GC record with an internal duplicate passes `pnl_check_lite` and lands duplicated pages in repnl. Explicitly documented as the "manageable compromise" (gc-get.c:1196–1222). |
| PAIR-F4 (V-019) | cursor-nav.c:349, 388; cursor-seek.c:673; tree-cutoff.c:243, 302 | low (assumption) | Raw `top_and_stash = (uint16_t)level` truncation discards `stash` without asserting `stash == 0`. Safe for settled cursors (stash > 0 exists only inside a single mutating call), and `level ≥ 0` holds at all call sites (verified: cursor-nav.c:425/469, cursor-seek.c:733/748–759, api-cursor.c:1063–1152 clamps) — but the invariant is unenforced. |
| PAIR-F5 (V-020) | tree-ops.c:587–589 | low (assumption) | Post-merge `top_and_stash = save_top − delta` requires the structural invariant `delta ≤ save_top` (root-collapse cannot remove more levels than the cursor's depth); only assert-checked. Leaf branch (580–584) keeps `top = save_top − 1` with `stash = 0`, consistent only for the single-level-collapse case. |
| PAIR-F6 (V-021) | spill.c:393–410 | informational | On `spill_page` failure the failing entry is compacted out of the DPL although it was neither written nor appended to the spill list; accounting (`dirtyroom`, `pages_including_loose`) includes it, so the DPL invariants stay internally consistent, and `bailout` (255) sets MDBX_TXN_ERROR making abort mandatory — the assert at 406 documents the window. Safe only as long as every failure path aborts the txn. |

## New invariants established during verification

- **PAIR-N1 (pnl_merge garbage-on-flag):** on a detected duplicate the merged array is weakly descending but retains *both* copies of the shared page and loses tail elements beyond the returned length; index 0 is never written (P_INVALID sentinel at pnl.c:248 caps `src_a` drain; write index bottoms at 1 while `src_b` remains). The return-value deficit is the only usable signal; the list must be discarded. Both CMOV and non-CMOV fallbacks are content-equivalent (no path divergence).
- **PAIR-N2 (gc-put temp-zone disjointness):** `pnl_need(&repnl, 2*loose_count+2)` makes the merge's write range `[1, size+c]` disjoint from the temp list zone `[size+c+2, alloclen−1]` — the `+2` slack is exactly the required margin; the `loose` pointer is computed after `pnl_need`, so realloc cannot invalidate it.
- **PAIR-N3 (dpl hole-freedom outside nested-commit):** `items[].ptr == nullptr` holes exist only transiently inside txn-nested.c:184–305 (commit merge); `txn_write`, spill, refund, sift, `page_touch_modifiable` may dereference `ptr` without a null check; `txn_dpl_check`'s null-tolerance (dpl.c:363) is defensive only.
- **PAIR-N4 (inner `subcur == nullptr`):** every inner cursor's `subcur` field is null (cursor-setup.c:311/501); the `!m3->subcur → continue` gate at cursor-mut.c:1122 is what guarantees `inner_gone_unconditional(m3)` (1124/1131) and `&m3->subcur->cursor` (1144) only ever see outer cursors.
- **PAIR-N5 (F_ISSET all-bits gate):** because `F_ISSET(w,f) == ((w & f) == f)` (utils.h:7), cursor_del's advance branch (cursor-mut.c:926–929) is unreachable for every non-filled *outer* state (poor/fresh 0xFFC4, Z2-eof 0x40–0x7F, 0x68) — those correctly return MDBX_ENODATA — and for filled inner inputs (pre-guarded at 958); the outer cursor's own contract is therefore never violated by the advance.
- **PAIR-N6 (height-bound chain):** `meta_validate` height < CURSOR_STACK_SIZE for GC/Main (meta.c:633/669) + nested height check (cursor-setup.c:375) + `cursor_push` bound ⟹ max legal `top = height−1 ≤ 30` and `top+stash ≤ 31`; every raw `top_and_stash` write in the audited files stays within `[0, 31]` with `stash ≥ 0` after the write.

**Overall: no INCONSISTENT verdicts. Pair 1 = CONSISTENT·A, Pair 2 = CONSISTENT·A, Pair 3 = CONSISTENT, Pair 4 = CONSISTENT·A.** The most actionable item is PAIR-F1 (cheap to close by checking the ignored `pnl_merge` return in gc_merge_loose, mirroring gc-get.c:1188–1189).
