# Verification wave 2, stream C (structure→consumer pairs) — proposed doc additions

Scope: the four stream-C pairs of wave 2:
1. `src/meta.c`/`src/meta.h` → `src/dxb.c` (`dxb_read_header`, `dxb_setup`, `dxb_sync_locked`);
2. `src/page-ops.c`/`src/page-ops.h` → `src/page-split.c` + `src/page-get.c`;
3. `src/cursor.h` predicates/primitives → `src/cursor-nav.c` + `src/cursor-seek.c`;
4. `src/mvcc-readers.c` → `src/refund.c` + the `gc-get.c` kick path.

Reuse from wave 1 / previous tranches: PAIR-N1..N6, L1..L8, PNL-slack/capacity/order,
DPL-cap/order, RKL-I1..I5, SORT-* (LEDGER + pairs-wave1.md). Cross-stream convergence:
stream A's `doc-additions-v2.md` INV-8 (two-phase meta reader safety) and INV-12 (#269
validate-after-read) independently confirm this stream's Pair-1 P6/P7 conclusions.

**Verdicts: all four pairs CONSISTENT·A** (no INCONSISTENT; assumptions and gaps are
registered as V-028..V-033 below — the V-id numbering continues after stream A's
V-023..V-027; note that stream B's `doc-additions-v6-b.md` re-used V-023..V-025 for
different findings — the registry needs a renumber pass at merge time).

---

## Pair 1: meta.c/meta.h → dxb.c

### Contracts consumed (meta.h/meta.c)
- `meta_tap(env)` (meta.c:133): three `meta_snap()` reads + `troika_fsm_map` unpack;
  post — `recent/prefer_steady/tail/valid/strict` consistent with the snapshotted
  txnids/steady bits; a page caught mid-`meta_update_begin..end` yields `txnid == 0`
  for that slot (`meta_snap` meta.c:21-30) and can never be selected as recent/
  prefer_steady while any valid page exists.
- `meta_recent/prefer_steady/tail(env, troika)` (meta.h:50-74): post — `is_steady`
  is `(fsm >> n) & 1`, i.e. derived from the *snapshotted* troika, never re-read from
  the page; `ptr_v/ptr_c` may be dereferenced only under the writer lock or behind a
  freshness re-check.
- `meta_update_begin/end` (meta.h:96-124): two-phase `txnid_b←0 → txnid_a←new →
  txnid_b←new`; pre(end): `txnid_a == txnid`; pre(begin): both old txnids < new.
- `meta_validate` (meta.c:416): returns SUCCESS only for a self-consistent meta
  (`txnid_a == txnid_b`, steady sign checksum, geometry/root/height bounds incl.
  `height < CURSOR_STACK_SIZE` for GC and Main).
- `meta_override` (meta.c:337): exclusive-lck-only direct page write (no two-phase —
  commented at meta.c:396-399); callers must re-tap the troika afterwards.

### Enumeration (all meta_* call sites in the three consumers)

| site | use | precondition check | verdict |
|---|---|---|---|
| dxb.c:121 `meta_validate` (read_header) | validate each 256-byte meta read | input is a *stable* image: double-read `memcmp(buffer, again)` loop (59-112, retryleft 42) + `txnid_a == txnid_b` inside validate ⟹ every mid-update state rejected | OK |
| dxb.c:135-160 `meta_bootid_match`/`meta_choice_recent/steady` on `unsafe_txnid`/`unsafe_sign` | latch selection | same stable-validated images only (`dest` is a latched validated copy) | OK |
| dxb.c:168/184/189 `meta_is_steady(dest)` | usable/PROPERLY gates | `dest` private copy | OK |
| dxb.c:279 `meta_init_triplet` | new-DB model fill | no live pages yet | OK |
| dxb.c:289 re-read of header under CHECKING | re-validate after creation | OK |
| dxb.c:501 `meta_tap` | post-mmap troika | before any recovery decisions | OK |
| dxb.c:510/551/564 `meta_validate_copy` | recovery-mode target / prefer_steady / recent | copies validated before any use | OK |
| dxb.c:518/623 `meta_eq_mask` | clash detection | troika-based; equal-txnid-different-steady pairs pass by design (not a clash) | OK |
| dxb.c:547-548/639 `meta_recent`/`meta_prefer_steady` | recovery loop + geo diff | `is_steady` from fsm bit; page deref under exclusive lck | OK |
| dxb.c:578 `meta_bootid_match(recent.ptr_c)` | same-boot crash detection | volatile deref under exclusive lck (writer is self) | OK |
| dxb.c:590 `header = clone` | bootid-match: adopt validated recent | **drops the user-amended geometry from dxb.c:389-395** — see V-028 | ASSUMPTION |
| dxb.c:616 `meta_override` (purge) | rollback: wipe recent to txnid 0 | followed by re-tap at 621 + `meta_txnid(recent.ptr_v)==0` ENSURE (622) | OK |
| dxb.c:674 `meta_set_txnid(&header, next)` | geo-update pending image | private image; `next = safe64_txnid_next(recent.txnid)` ⟹ head.txnid != pending ⟹ tail path in sync_locked | OK |
| dxb.c:675 `dxb_sync_locked(..., &header, &troika)` | apply amended geometry | troika freshly re-tapped after any override/rollback | OK |
| dxb.c:697-711 `meta_is_used`/`meta_is_steady`/`constmeta_txnid`/`meta_override` | format-upgrade loop | live-page reads under exclusive lck; re-tap at 711 after each override | OK |
| dxb.c:813/886/980/1034 `meta_recent`/`meta_prefer_steady`/`meta_tail` (sync_locked) | head/tail selection | troika-based; ptr_c derefs under writer lock | OK |
| dxb.c:896-906 `meta_set_txnid` + `coherency_check_meta(pending)` | shrink force-forward | private pending; only reachable with no active write txn (ENSURE 899) | OK |
| dxb.c:1004/1013 `meta_sign_as_steady`/poke WEAK | pending sign per sync outcome | private pending | OK |
| dxb.c:1071/1093 `meta_update_begin/end` (WRITEMAP tail write) | invalidate + commit tail | `constmeta_txnid(target) < pending->unsafe_txnid` ENSURE (1066) satisfies begin's pre; mid-window page reads `txnid == 0` via meta_snap ⟹ never selected by fresh taps | OK |
| dxb.c:1101 sign-only write (target == head) | steady promotion of the head | gated by `legal4overwrite` + `!head.is_steady && meta_is_steady(pending)`; all sign-only-capable callers pass a whole-page copy of head (env.c:183 with fence+memcmp stability double-read; defrag.c:153; gc-get.c:1362) ⟹ trees/canary/geometry/pages_retired/bootid/dxbid equal by construction | OK (see V-id note below) |
| dxb.c:1153 `coherency_check_written` | post-write self-check with retry | #269 protocol | OK |
| dxb.c:1166-1169 `meta_tap` + troika propagation | refresh writer troika after meta write | updates basal + all nested | OK |

**`unsafe_txnid` discipline — where unsafe reads are safe.** A raw (non-two-phase)
read of `txnid_a/txnid_b/sign` is safe exactly at: (a) `dxb_read_header` — after the
double-read stability loop, and only through `meta_validate`'s `a==b` gate (any
mid-update interleaving yields `a != b` and the page is skipped); (b) private copies
(`dest` latch, `header`, `pending`, the `undo_meta` image); (c) live-page derefs under
the writer mutex (the writer is the only meta mutator). Everywhere else the tree uses
either a fresh tap + `meta_should_retry` loop (api-txn.c:49-58, 538-547) or the
begin-time copy with a post-read `meta_txnid(head.ptr_v) != head.txnid` re-check
(txn-ro.c:161, ro_seize). `env.c:183` additionally does fence+`memcmp` stability
double-read before consuming a head copy.

**P6 — the CHECKING-build provoke memset (dxb.c:1073-1080, 0xCC over
`trees.gc.root..sign` between begin/end).** No reader path observes it as
authoritative data: readers copy `trees/geometry/canary` at begin via
`coherency_fetch_head` (coherency.c:110-124) and afterwards dereference meta pages
only through a fresh `meta_tap` (an invalidated page has `txnid == 0` and cannot be
selected while another page is valid) or a re-checked begin loop. A reader holding a
*stale* troika whose head became the target sees either `meta_txnid != head.txnid`
(ro_seize) or a changed fsm/txnid set (`meta_should_retry`) and retries. The memset
deliberately stops before the pagesize overlay (`trees.gc.flags/height/dupfix_size`
union) — documented at dxb.c:1074-1075. Cross-confirms stream A INV-8.

**legal4overwrite sufficiency (dxb.c:1017-1020).** The memcmp set is
`trees + canary + geometry`; not compared: `pages_retired`, `bootid`, `dxbid`,
`magic/reserve16/validator_id/extra_pagehdr`, `sign` (deliberately), `txnid_b`
(equal by the head-snap invariant). Sufficiency holds **by construction at the
current call sites** (whole-page copies), not by the gate itself: a future caller
that mutates `pages_retired` while keeping the txnid would silently lose the change
in the sign-only path. Proposed contract below; registered as an assumption, not a
defect.

**Verdict: CONSISTENT·A** (assumptions: legal4overwrite by-construction;
bootid-match geometry drop V-028).

---

## Pair 2: page-ops.c/h → page-split.c + page-get.c

### Contracts consumed (page-ops.h/page-ops.c)
- Page-state machine (`page-ops.h:14-53`): `is_frozen` = `mp->txnid < txn->txnid`;
  `is_spilled` = `== txn->txnid`; `is_shadowed` = `> txn->txnid`;
  `is_modifiable` = `== txn->front_txnid`; `is_tmp` = tmp-signature.
  **Load-bearing semantics (new invariant PAIR-N9):** `front_txnid = txnid + 1` iff
  malloc-dirty (txn.c:244), nested `front = parent.front + 1` (txn-nested.c:345),
  and **nested write txns are forbidden under MDBX_WRITEMAP**
  (txn-nested.c:458-461 `txn_nested_create`) — together these make the four
  predicates a correct partition in both modes; in malloc mode the current txn's own
  heap pages are `is_shadowed` by design (this is exactly what `page_check`'s
  out-of-map gate at page-get.c:60-68 relies on to accept heap dirty pages while
  rejecting stray map addresses).
- `page_new/page_new_large` (page-ops.c:6-48): post — flags set, `lower=0`,
  `upper = ps-PAGEHDRSZ`, tree counters updated; `page_new_large` sets
  `pages = npages` and asserts `!z_inner`.
- `page_dirty` (page-ops.c:712): pre — not ro; post — `mp->txnid = front_txnid`,
  dpl append or `writemap.dirty_npages += npages`; dirtyroom==0 → loose-purge or
  `MDBX_TXN_FULL`.
- `page_shadow_alloc/release` (page-ops.c:286-343): malloc/free pool with
  `page_next` chaining; checking builds set `pgno=0, flags=P_BAD`.
- `page_check` (page-get.c:50-387): full node-level validation.
- `page_get_{any,three,large}` (page-get.c:514-524): ILL-mask dispatch —
  `three` = P_ILL_BITS|P_LARGE (forbids large, accepts branch/leaf/leaf2),
  `any` = P_ILL_BITS (accepts all), `large` = P_ILL_BITS|B|L|D (requires P_LARGE).

### Enumeration — page-split.c

| site | use | precondition check | verdict |
|---|---|---|---|
| page-split.c:25 `page_new(mc, P_BRANCH)` | root split | entry invariant `stash == 0` (cursor-mut.c:19 gate; recursive entries carry stash == recursion depth, see PAIR-N7) | OK |
| page-split.c:32-40 root-split stack shift | `pg[i+1]=pg[i]` for `i = stash..0` | writes `pg[stash+1] ≤ pg[31]`: at the root-split moment `top == 0` and `stash ==` recursion depth ≤ outer `top ≤ 30` (PAIR-N7) | OK |
| page-split.c:105 `page_new(mc, mp->flags)` | sister page | flags of a valid leaf/branch | OK |
| page-split.c:196 `page_shadow_alloc(1)` + :214 tmp signature | tmp copy of mp | `is_tmp` ⟹ `is_shadowed` ⟹ accepted by page_check's out-of-map gate (both modes); node_add_* never calls page_touch on it (no page-ops state checks inside node.c) | OK |
| page-split.c:291/299, 335/353, 360/363, 390/394 `cursor_enroot/undo_enroot` | parent-level work | wave-1 verified pairing; undo skipped on failure paths deliberately (txn error ⟹ abort, `be_poor` at 565 resets top/stash) | OK |
| page-split.c:403-408 `stash += 1` + bound check | stash the source page | explicit `top+stash >= CURSOR_STACK_SIZE → CURSOR_FULL` before `pg[top+stash]` write | OK |
| page-split.c:557 `page_shadow_release(tmp)` | all exits | single `exit:` label covers success + bailout | OK |
| MDBX_SPLIT_REPLACE (:75, :523) | suppress pure-left / ki-increment for replacement | both callers pass it exactly for re-insertion (cursor-mut.c:665, tree-ops.c:930) | OK |

### Enumeration — page-get.c

| site | use | precondition check | verdict |
|---|---|---|---|
| page-get.c:444-489 `page_get_unchecked` | `pgno < first_unallocated` gate then map/dpl resolution | `pgno2page` bound: `first_unallocated ≤ geo.now ≤ dxb_mmap.current ≤ limit` (txn-basal.c:180-181 asserts); dpl scan only for `(txn_ro_flat|WRITEMAP)==0`; `pgno != r.page->pgno` → bad_page (#269) | OK |
| page-get.c:389-432 `check_page_header` ILL dispatch | type/txnid/geometry gates | early `return` at 391-393 on `flags & ILL` prevents the upper/lower check from ever running on a type-mismatched (e.g. large-under-`three`) page — verified per variant (see PAIR-N8) | OK |
| page-get.c:80-85 / :421-427 large `pages` bounds | `1 ≤ npages < MAX_PAGENO/2`, `pgno+npages ≤ first_unallocated` | no uint32 overflow (0x7FFFFFFF + 0x3FFFFFFF < 2³²) | OK (P2) |
| page-get.c:236-248 / node.c:245-250 `largechunk_npages` cross-check | `lp.page->pages ≥ largechunk_npages(dsize)` | `largechunk_npages = floor((bytes+PAGEHDRSZ-1)/ps)+1 ≥ ceil` — allocation always sufficient; over-allocation → `poor_page` (info) only for non-GC trees | OK |
| value-size caps (cogs.h:114-140) | `valsize_max ≤ PAGELIST_LIMIT/4` pages | ⟹ `largechunk_npages(lmax) < MAX_PAGENO/2` ⟹ consistent with page_check's npages bound and with pnl limits | OK (P2) |
| page-get.c:50-387 `page_check` | see completeness below | rc only warns/flags; sets `MDBX_TXN_ERROR` via `check_page_complete` | OK |

**page_check completeness — what it does NOT check (and who guarantees):**
1. `page->txnid` page-state consistency — guaranteed by `check_page_header`
   (page-get.c:407-409, runs before page_check in `check_page_complete`) and by the
   CoW discipline (page_touch_unmodifiable) for pages reachable only through cursors.
2. `N_TREE`/`N_TREE|N_DUP` `tree_t` payload (root/height/counts/mod_txnid) —
   guaranteed lazily: subtree traversal goes through `page_get` (pgno-range gate) and
   `cursor_setup` page_checks; top-level trees additionally by `meta_validate` +
   `coherency_check`. A crafted `tree_t` inside a node is not rejected at the
   containing page.
3. Large-page payload bytes — never parsed; guaranteed by the writer only.
4. Branch-node self-reference (`node_pgno(node) == mp->pgno`) — not checked;
   misdescent is bounded by `cursor_push`'s CURSOR_FULL and page_get validation, no
   memory unsafety.
5. `entries[i]` beyond `nkeys` (derived from `lower`) — self-consistent by
   construction; DUPFIX `numkeys` bounded per-item at page-get.c:142.

**Verdict: CONSISTENT·A** (PAIR-N7/N8/N9 established; V-028 unrelated; no
violations found in the state machine at any site).

---

## Pair 3: cursor.h predicates/primitives → cursor-nav.c + cursor-seek.c

### Contracts consumed (cursor.h)
- Zones Z0-Z3 (cursor.h:88-171, STATIC_ASSERT); `is_filled` = `(uint8_t)flags <
  z_eof_hard` (208); `is_pointed` = `top ≥ 0 == combo_state ≥ 0` (187); `is_hollow`
  = `flags < 0` (194) with the `top ≥ 0 ∧ ki < numkeys` assert when not hollow;
  `eof_hard_clear` (239-244) clears `z_eof_hard` only when pointed at an element and
  not tail-deleted; `F_ISSET` all-bits (PAIR-N5).
- `cursor_push/eject/enroot/undo_enroot` (cursor.h:386-426) — wave-1 verified.

### Enumeration — cursor-nav.c

| site | use | precondition check | verdict |
|---|---|---|---|
| cursor-nav.c:11-50 `sibling` | pre `top ≥ 1`; enroot/undo; `page_get` + `cursor_push` | NOTFOUND → undo + propagate; other errors → `be_poor` | OK |
| :21 `ki+1 >= numkeys` / `ki == 0` | sibling-of-sibling recursion | bounds hold (ki < numkeys maintained) | OK |
| :39 `page_get(mc, node_pgno(node), &mp, mp->txnid)` | child fetch with parent-txnid front | ILL = three-variant (branch child ⟹ any non-large type allowed) | OK |
| :253-264 `cursor_sibling_left` NOTFOUND | ki = 0, stay on current page | flags unchanged ⟹ filled state preserved; PREV-at-start returns NOTFOUND with the cursor on the first row (valid record, R3/R4 ok) | OK |
| :266-283 `cursor_sibling_right` NOTFOUND | ki = nkeys-1, flags = `z_eof_soft|z_eof_hard|(flags & z_clear_mask)` | **the uniform past-end state (a)** | OK |
| :54-124 `cursor_bring` | `check_leaf_type` gate; `nkeys > ki` asserts; dupfix/dupsort setup; `be_filled` | eof ⟹ `ki == nkeys-1` (72) | OK |
| :127-138 `cursor_brim` | `tree_search(Z_FIRST/Z_LAST)` when top != 0 | nkeys > 0 asserted | OK |
| :142-251 `cursor_step` | is_poor/fresh handling (161-171); state dispatch (178-219); sibling moves | poor: fresh → first/last; else poor-mark + `z_after_delete ? NOTFOUND : ENODATA` (R2 uniform); eof_hard forward → NOTFOUND; eof_hard backward → last row; after_delete forward → bring | OK |
| :347-384 / :386-421 `cursor_forward/backward_at` | raw `top_and_stash = (uint16_t)level` | PAIR-F4/V-019 (wave 1) — same sites, unchanged | OK·A |
| :423-465 / :467-512 `cursor_scroll_*` | dup-aware long scroll | `level < tree->height || !subcur` split; inner recursion gets `level - height ≥ 0` | OK |

### Enumeration — cursor-seek.c (ops dispatcher sampling; ≥8 ops verified against
the §5 outcomes table of libmdbx-invariants.md)

| op | site | observed outcome state | vs §5 table | verdict |
|---|---|---|---|---|
| `GET_CURRENT` (282-325) | not-filled → hollow: ENODATA; `ki ≥ numkeys`: NOTFOUND; after_delete → NEXT_NODUP; inner after_delete → NEXT_DUP; inner_tail_deleted → NEXT; eof_soft on last row → reads it | matches the table's GET_CURRENT column for all listed rows | OK |
| `GET_BOTH`/`GET_BOTH_RANGE` miss (208-218 + target_not_found 102-112) | inner seek miss ⟹ `mc->flags |= z_hollow` (+inner), NOTFOUND | table "всё запрещено / ENODATA / ENODATA": cursor_del entry gate `F_ISSET(flags, z_inner|z_eof_hard)` false for the hollow outer (PAIR-N5) ⟹ ENODATA ✓ | OK |
| `SET_RANGE` past-end | **two distinct states**: (a) via `cursor_sibling_right` NOTFOUND — ki = nkeys-1, flags 0x60; (b) via the fast path dxb… cursor-seek.c:100+115 — ki = nkeys, flags 0x60 | table claims one state "z_eof_soft on the last row"; (b) is beyond the last row: `GET_CURRENT` → NOTFOUND (287) while (a) → reads the last row — **R1 uniformity gap** → V-029 | DISCREPANCY (low) |
| `TO_KEY_EQUAL` miss (516-517) | rc==SUCCESS (landed on a greater key, exact=false) ⟹ return NOTFOUND with the cursor **filled on the neighbor record**; rc==NOTFOUND (past-end) ⟹ eof state | table row describes only the past-end sub-case ("z_eof_soft on the last row, del deletes it"); the landed-on-neighbor sub-case leaves `GET_CURRENT`/`del` acting on the neighbor → V-030 | DISCREPANCY (low/doc) |
| `PREV` after z_eof_soft (cursor_step 234-244) | `--ki` → second-to-last row | consistent with "z_eof_soft is readable, prev moves off" | OK |
| `PREV` after z_eof_hard (215-217) | ki = nkeys-1 → bring → last row | matches doc comment + table | OK |
| `NEXT` after z_eof_hard (212-213) | NOTFOUND, no movement | Z2 CRUD-forbidden ✓ | OK |
| `NEXT_DUP` at inner tail (195-196) | returns inner NOTFOUND; outer stays filled; `eof_hard_clear` both (cursor_ops:422-423) | re-NEXT_DUP deterministic NOTFOUND; del of the current key legal | OK |
| `NEXT_NODUP` after z_after_delete at page end (207-210 → 223-233 → sibling_right NOTFOUND) | past-end state (a) | OK |
| `FIRST` on poor fresh (161-167) | auto-position first | OK |
| `TO_PAIR_EQUAL` miss among duplicates (620-630) | explicit uniform past-the-tail inner state: `ki = numkeys`, `flags = (flags & z_inner) \| z_eof_soft \| z_eof_hard` | table "единое past-the-tail состояние"; GET_CURRENT → NOTFOUND (ki ≥ numkeys), del → ENODATA ✓ | OK |
| `SET_LOWERBOUND/UPPERBOUND` (454-495) | SET_RANGE + inner refinement; RESULT_TRUE for "greater found" | consistent with R4 | OK |
| `GET_MULTIPLE`/`NEXT_MULTIPLE` (359-399) | requires `inner_filled`; ki pinned to last dup | OK |

**cursor_distance/distribute arithmetic (P2).** `cursor_distance_at`
(cursor-seek.c:670-693): `iter_ki = ki + (eof_hard && level+1 == height)` assumes the
eof_hard position is at `ki == nkeys-1` (state a). The SET_RANGE past-end state (b)
(`ki == nkeys`) yields `iter_ki = nkeys+1` ⟹ same-page distance over-counts by 1 and
cross-page distance starts at `-1` (assigned to `size_t distance` — wrap). Consumers
absorb it (`cursor_distribute`'s overshoot → NOTFOUND → `be_poor` +
`MDBX_RESULT_TRUE`; release-safe, no memory unsafety), but `ASSERT(tdr.distance > 0)`
(cursor-seek.c:844) trips in checking builds and the split counts skew by one —
V-032. `cursor_leaf_amount` bounds: `dsize ≥ sizeof(tree_t)` / `> PAGEHDRSZ` gated by
`!MDBX_DISABLE_VALIDATION` — the release-build crafted-data compromise is the
documented V-022 analog. Level bounds: raw `(uint16_t)level` sites are the wave-1
V-019 set; `cursor_distance_at` is called with `level - tree->height ≥ 0` from the
`level ≥ tree->height` branches (733/748/759/796) — no new violations.

**Verdict: CONSISTENT·A** (assumptions V-029/V-030/V-032 — state-uniformity gaps in
the seek dispatcher; no precondition violations).

---

## Pair 4: mvcc-readers.c → refund.c + gc-get.c kick path

### Contracts consumed (mvcc-readers.c)
- `mvcc_snapshot_oldest_rw` (108-204): **the only writer of `cached_oldest_txnid`**
  (190). Post: `result.oldest_txnid ≤ steady.txnid` (initialized to steady at 150 and
  only lowered to reader txnids ≥ `trusted_oldest`); monotone rise except the
  deliberate distrust reset; `distrust_cached_oldest = prev > steady.txnid` ⟹ forced
  full rescan with `trusted_oldest = MIN_TXNID` (125-128) — forged-high cache values
  are recovered, and the demote branch (168-175) cannot fire for legitimate readers
  because `trusted_oldest = MIN_TXNID` in that pass.
- `mvcc_snapshot_largest` (206-224): double-load consistency check (215-217) +
  `snap_txnid <= env->basal_txn->txnid` filter (218) ⟹ forged/lagging slots cannot
  inflate the largest live snapshot.
- `mvcc_kick_laggards` (378-512): post — `return orsi.oldest_txnid > straggler`;
  **every true return has updated `cached_oldest_txnid`** (its internal
  `mvcc_snapshot_oldest_rw(txn, false)` always full-scans and stores on change,
  186-191) ⟹ the caller's re-detent strictly advances (PAIR-N10).

### Enumeration

| site | use | precondition check | verdict |
|---|---|---|---|
| gc-get.c:1036-1043 `txn_gc_detent` + FATAL guard | detent refresh + `detent > txn_basis_snapshot` | `detent ≤ steady.txnid ≤ head-at-begin = txn.txnid - 1 = basis` ⟹ guard unreachable in legit flows | OK |
| gc-get.c:1333-1396 kick/steady block | `detent == prefer_steady.txnid` gating; `meta_wipe_steady` (re-taps troika, meta.c:250-252); `meta = *recent.ptr_c` steady-sync (whole-page copy ⟹ sign-only path legal, Pair 1) | OK |
| gc-get.c:1390-1392 rdt_refresh_flag + re-detent | flag ⟹ refresh; kick-true ⟹ cache advanced ⟹ detent strictly increases ⟹ cycle bounded by txnid range (MAX_TXNID) | no infinite loop | OK |
| gc-get.c:1399 `mvcc_kick_laggards(txn, detent)` | kick contract above | OK |
| gc-put.c:961-966 `gc_reserve4return` release-active check | `reservation_id ≤ cached_oldest` | reservation ids chosen ≤ detent (true oldest, never the forged cache) and cached only rises ⟹ holds; forged-low cache ⟹ `MDBX_PROBLEM` (fail-safe) | OK·A (V-031) |
| gc-put.c:482/514 tASSERTs | debug-only | OK |
| refund.c:173-201 `txn_refund` | tail-only refund: `refund_reclaimed` pre `MOST == first_unallocated-1` checked in-loop (180); `refund_loose` dirtylist/spill accounting (PAIR-N2/N3 reuse) | **no `cached_oldest_txnid` dependence at all** — refund is writer-local | OK |
| page-ops.c:550-593 / :658-661, page-ops.h→page_dirty:747-750 refund call sites | guard `MDBX_PNL_MOST == first_unallocated-1` before calling txn_refund | OK |

**txn_refund cannot shrink below any live snapshot (new invariant PAIR-N11).** The
refund path fires only for `pgno + npages == txn->geo.first_unallocated` (the tail
allocated by this/parent txn, never committed). Any reader slot's
`snapshot_pages_used` (txn-ro.c:162, written after the txnid re-check) equals the
pre-txn `first_unallocated ≤ pgno` ⟹ no live reader references the refunded range.
The file-shrink side uses `mvcc_snapshot_largest` + `dxb_shrink_bottom`
(dxb.c:792-804) whose `bottom > largest_pgno` keeps every live reader's range mapped
— verified consistent (cross-confirms stream A INV-11).

**Unguarded `cached_oldest_txnid` consumers (V-031).** Raw reads without the
steady-bound trust guard: `ro_seize` (txn-ro.c:195-205 — forged-high cache ⟹ 42
re-taps ⟹ `MDBX_PROBLEM` for every reader start until a writer's rescan heals the
cache), release-fatal `ENSURE_OBJ` at txn-ro.c:121, txn-ro.c:284 and
txn-basal.c:253, `mvcc_largest_this` (mvcc-readers.c:240 — poison-coverage only),
gc-get.c:1438 (log only). All fail-safe (no memory-safety impact), but the
forged-value threat model that motivated the distrust protocol (comment at
mvcc-readers.c:117-124) is not applied on the reader/paranoia side — the RW path
recovers where the RO path hard-fails.

**hsr-callback decline loop (V-033).** `mvcc_kick_laggards`' do-while
(mvcc-readers.c:394-502): a callback that always returns 0 (declines) neither breaks
nor advances ⟹ the loop runs `INT_MAX` iterations, each a full reader-table scan +
callback — an effectively unbounded writer stall under the writer mutex. Bounded by
`INT_MAX` (terminates), matches the documented hsr contract (a giving-up callback
returns < 0), but practically unbounded — liveness hazard of the same family as V-004.

**Verdict: CONSISTENT·A** (PAIR-N10/N11 established; V-031/V-033 noted).

---

## 1. Proposed additions to `docs/engineering/libmdbx-invariants.md`
(numbering continues after stream A's §8-12 and stream B's INV-T*)

### INV-C1 (PAIR-N7) — page_split stack bound at the root split
At every `page_split` entry, `top + stash ≤ 30` with `stash` equal to the
parent-split recursion depth (outer entries have `stash == 0`, asserted at
cursor-mut.c:19). The root-split shift loop (page-split.c:32-40) writes
`pg[1..stash+1] ≤ pg[31] < CURSOR_STACK_SIZE`; the tmp-page stash push
(page-split.c:403-408) carries its own explicit bound. Chain: `meta_validate`
`height < CURSOR_STACK_SIZE` (meta.c:633/669) ⟹ `top ≤ 30` (PAIR-N6) ⟹ the shift
never leaves the stacks.

### INV-C2 (PAIR-N8) — page_get ILL-mask dispatch
`page_get_three` accepts exactly branch/leaf/leaf2 (P_LARGE forbidden — the "three"
are the three *non-large* types); `page_get_any` accepts all types incl. large;
`page_get_large` accepts only P_LARGE. `check_page_header` returns early on
`flags & ILL` (page-get.c:391-393), so the lower/upper-vs-pages union-dispatch
(page-get.c:411-430) never inspects the wrong overlay of a type-mismatched page.

### INV-C3 (PAIR-N9) — front_txnid page-state machine
`front_txnid = txnid + 1` iff the txn is a malloc-dirty (non-WRITEMAP, non-RO) txn
(txn.c:244); nested `front = parent.front + 1` (txn-nested.c:345); nested write
txns are rejected under MDBX_WRITEMAP (txn_nested_create, txn-nested.c:458-461).
Consequently `is_frozen/is_spilled/is_shadowed/is_modifiable` partition every page
state correctly in both modes, and in malloc mode the current txn's own heap pages
are `is_shadowed` **by design** — `page_check`'s out-of-map gate
(page-get.c:60-68) relies on that to accept heap dirty pages while rejecting stray
in-map addresses. `page_wash`/`page_dirty`'s WRITEMAP/AVOID_MSYNC/dirtylist
asserts are corollaries.

### INV-C4 (PAIR-N10) — cached-oldest discipline
`lck->cached_oldest_txnid` is written only by `mvcc_snapshot_oldest_rw`
(mvcc-readers.c:190); the stored value is `≤ steady.txnid` by construction
(initialized to steady, lowered only to reader txnids) and monotone rising except
the deliberate distrust reset. `distrust = prev > steady.txnid` forces a full rescan
with `trusted_oldest = MIN_TXNID`, so the "kick stuck reader" demote branch
(168-175) can never fire for a legitimate reader. Corollaries: `env->gc.detent ≤
steady.txnid ≤ txn_basis_snapshot(txn)` ⟹ the gc-get FATAL guard (gc-get.c:1040) is
unreachable; every `mvcc_kick_laggards == true` return has advanced the cache ⟹ the
kick/detent retry cycle strictly increases the detent and is bounded by MAX_TXNID.

### INV-C5 (PAIR-N11) — tail-refund reader safety
`txn_refund`/`page_retire_ex` refund only the uncommitted tail
(`pgno + npages == first_unallocated`); no live reader's `snapshot_pages_used` can
exceed the refund target (readers snapshot the pre-txn first_unallocated, and an
uncommitted allocation is invisible to any snapshot taken meanwhile). File-shrink
protection is separate: `mvcc_snapshot_largest` (double-load ABA check +
`snap_txnid <= basal.txnid` filter) feeds `dxb_shrink_bottom`, whose `bottom` stays
above the largest live snapshot.

### Amendment to §5 outcomes table (R1)
Add a footnote to the `SET_RANGE` past-end and `TO_KEY_EQUAL` miss rows: the past-end
state has the two encodings (a) `ki = nkeys-1, flags z_eof_soft|z_eof_hard` (via
`cursor_sibling_right` NOTFOUND) and (b) `ki = nkeys`, same flags (fast path,
cursor-seek.c:100+115); `GET_CURRENT` reads the last row under (a) and returns
`MDBX_NOTFOUND` under (b); `del` is uniform (`MDBX_ENODATA`); the `TO_KEY_EQUAL`
miss additionally leaves the cursor filled on the next-greater record when
`SET_RANGE` landed on one (V-029/V-030).

## 2. Proposed additions to `docs/engineering/module-interfaces.md`
(primitive contracts, structure→consumer format)

### meta.h / meta.c
- `meta_tap`: pre — none (safe under concurrent commit); post — troika slots of
  pages caught mid-update read `txnid == 0`; `recent/prefer_steady/tail/valid`
  mutually consistent with the snapshot; consumers must re-tap (or
  `meta_should_retry`) after any own meta mutation (`dxb_sync_locked`,
  `meta_wipe_steady`, `meta_override`).
- `meta_recent/prefer_steady/tail`: `is_steady` is the *snapshot's* fsm bit, not a
  live read; `ptr_v/ptr_c` deref requires writer-lock exclusivity or an external
  freshness re-check; safe deref patterns in-tree: begin-time copy
  (`coherency_fetch_head`) + `meta_txnid(head.ptr_v) == head.txnid` re-check
  (txn-ro.c:161), fresh-tap + `meta_should_retry` loops (api-txn.c:49/538),
  fence+memcmp double-read (env.c:183).
- `meta_update_begin/end`: begin pre — both current txnids < new; end pre —
  `txnid_a == new`; WRITEMAP-only (non-WRITEMAP writes the whole image via pwrite
  with the `undo_meta` fallback, dxb.c:1122-1148); between begin and end the page is
  globally invisible to the troika (`meta_snap` ⟹ 0).
- sign-only head promotion (`dxb_sync_locked` target == head): contract — `pending`
  differs from the head page **only** in `sign`; the `legal4overwrite` memcmp set
  (trees/canary/geometry) is necessary but not sufficient — `pages_retired`/`bootid`/
  `dxbid` equality is by construction (whole-page-copy callers). Any new caller must
  preserve that construction or extend the memcmp set.

### page-ops.h / page-ops.c
- page-state predicates: see INV-C3 (front_txnid machine + nested-WRITEMAP
  prohibition as the load-bearing gate).
- `page_dirty`: pre — write txn, not ro; post — dpl entry (npages honored) or
  `writemap.dirty_npages += npages`; `MDBX_TXN_FULL` when dirtyroom is exhausted and
  no loose page can be purged; caller must treat the page as washed/failed on error.
- `page_shadow_alloc`/`page_shadow_release`: strict pair per pointer; release
  accepts any npages (pool only for npages == 1); checking builds poison the payload.
- `page_check`: validates pgno/address/flags/type/lower-upper/nkeys/per-node
  layout/order/sizes and (for N_BIG) the referenced large-page's `pages` field;
  does NOT validate `page->txnid` (that is `check_page_header`'s job), N_TREE
  `tree_t` payloads (lazy — subtree traversal), large-page payload bytes, or branch
  self-reference — see the guarantee list in the Pair-2 section above.
- `largechunk_npages(bytes)` = `floor((bytes+PAGEHDRSZ-1)/ps) + 1 ≥ ceil((bytes+19)/ps)`
  — always ≥ the minimum; value-size caps (`valsize_max ≤ PAGELIST_LIMIT/4` pages)
  keep `npages < MAX_PAGENO/2` (page_check's bound) and interplay with
  PAGELIST_LIMIT consistently.

### cursor.h / cursor-nav.c / cursor-seek.c
- Zone contract: see the comment before `enum cursor_state`; `eof_hard_clear` clears
  `z_eof_hard` only when pointed at an element and not tail-deleted — required after
  every NEXT-family op before rebalance-touching operations (cursor_ops:422-423).
- `cursor_sibling_right/left` NOTFOUND post: right — uniform past-end state
  (ki = nkeys-1, `z_eof_soft|z_eof_hard`); left — ki = 0 with flags unchanged
  (cursor stays on a valid record).
- `cursor_step` failure states: poor non-fresh → poor-mark + (`z_after_delete` ?
  `MDBX_NOTFOUND` : `MDBX_ENODATA`); eof_forward → NOTFOUND; eof_hard backward →
  last row; at-start backward → NOTFOUND on the first row.
- `cursor_seek` miss states: exact-match ops (`SET`, `GET_BOTH`, `GET_BOTH_RANGE`)
  ⟹ `z_hollow` everywhere; `SET_RANGE` past-end ⟹ one of the two encodings
  (INV-C amendment above); exact=false with a landing ⟹ filled on the landing.
- `cursor_distance_at`: the `eof_hard` rank adjustment assumes `ki == nkeys-1`;
  callers must not pass the `ki == nkeys` past-end encoding (V-032), or must
  normalize first.

### mvcc-readers.c / refund.c
- `cached_oldest_txnid`: single-writer (`mvcc_snapshot_oldest_rw`), value ≤
  `steady.txnid`, monotone (INV-C4); raw readers without the trust guard are
  restricted to fail-safe/paranoia uses (V-031 lists them).
- `mvcc_kick_laggards(txn, straggler)`: returns `oldest > straggler`; every true
  return persisted an advanced cache ⟹ the caller's re-detent strictly advances;
  termination bound = txnid range (plus the INT_MAX inner cap, V-033 for the
  declining-callback corner).
- `txn_refund`: writer-local; never consults reader state; tail-only
  (`pgno+npages == first_unallocated`); pre for `refund_reclaimed` is re-checked
  in-loop (`MDBX_PNL_MOST == first_unallocated-1`); safe against all live readers
  (INV-C5).

## 3. Proposed additions to `docs/engineering/test-coverage.md`

1. **V-029/V-030 (positional state audit)**: extend the positional/regression test
   for §5 outcomes: after a `SET_RANGE` miss beyond the end, assert `GET_CURRENT`
   behavior for both encodings (fast-path entry from the last row vs tree-search
   entry from mid-page), and after a `TO_KEY_EQUAL` miss assert the documented
   position; golden-update per the "любое изменение таблицы исходов" clause.
2. **V-032**: checking-build test (or static assert) that `mdbx_cursor_distance`
   with a past-end `begin` cursor (fast-path encoding) is either normalized or
   documented; add `ASSERT(distance > 0)` triage to the checking CI matrix.
3. **V-031**: adversarial lck test — write `cached_oldest_txnid = txnid_max` into
   the lock file while a reader/writer mix runs; assert the RW path recovers
   (distrust/rescan) and reader starts either succeed or fail with a documented
   code, never corrupt; pin the ENSURE_OBJ sites' expected behavior.
4. **V-033**: hsr-callback contract test — a callback that returns 0 forever must
   not hang the writer beyond the documented bound; either bound the loop or
   document `INT_MAX` as the contract and test a giving-up (< 0) callback.
5. **V-028**: geometry test — pre-configure geometry (lower/upper/grow/shrink) then
   re-open exclusively after a same-boot crash (bootid-match path); assert the
   pre-configured geometry is applied or document that it is deliberately deferred
   to the next explicit `mdbx_env_set_geometry`.
6. **page_split depth stress**: tree of height ≈ CURSOR_STACK_SIZE-1 with cascading
   parent splits (PAIR-N7 bound) under `MDBX_CHECKING` — covers the `pg[stash+1]`
   bound and the CURSOR_FULL stash guard.
7. **INV-C3 regression**: keep the nested-WRITEMAP prohibition test
   (`mdbx_txn_begin` nested under `MDBX_WRITEMAP` ⟹ `MDBX_INCOMPATIBLE`) — it is
   load-bearing for the whole page-state machine.

## 4. Findings referenced above (V-ids continue the global registry)

| V-id | file:line | severity | description |
|---|---|---|---|
| V-028 | dxb.c:590 (with 389-395, 640-644) | low | `dxb_setup` bootid-match branch (`header = clone`) discards the user-amended geometry prepared at dxb.c:389-395; the subsequent geo-diff (640-644) then sees no delta and the preconfigured lower/upper/grow/shrink are not written to the meta. Only `geometry.now` self-heals via dxb.c:633-637 (mmap-current check). Reachable: exclusive RDWR re-open after a same-boot crash with pre-open geometry configured. Fix shape: merge the amendment into `clone` instead of overwriting `header`. |
| V-029 | cursor-seek.c:100+115 vs 152-159/280 | low | `SET_RANGE` past-end leaves two distinct failure states depending on the entry path: (a) via `cursor_sibling_right` NOTFOUND — `ki = nkeys-1`, flags 0x60; (b) via the fast path — `ki = nkeys`, flags 0x60. `GET_CURRENT` returns the last row's data under (a) but `MDBX_NOTFOUND` under (b); `del`/`NEXT`/`PREV` are uniform. Violates the R1 uniformity claim as recorded in the §5 table (the table's state "z_eof_soft на последней строке" does not hold for (b)). |
| V-030 | cursor-seek.c:516-517 | low (doc) | `TO_KEY_EQUAL` miss with `rc == MDBX_SUCCESS` (SET_RANGE landed on a greater key) returns NOTFOUND while leaving the cursor **filled** on that neighbor record — `GET_CURRENT` reads it and `del` deletes it. The §5 table's TO_KEY_EQUAL row ("z_eof_soft on the last row; del deletes it") covers only the past-end sub-case. Either the table needs a second sub-row, or the op should normalize the miss state. |
| V-031 | txn-ro.c:195-205, 121, 284; txn-basal.c:253; mvcc-readers.c:240 | low | Forged-high `cached_oldest_txnid` (externally-writable lck mapping — the exact threat model of the distrust protocol at mvcc-readers.c:117-128) is recovered by `mvcc_snapshot_oldest_rw`, but raw consumers hard-fail: `ro_seize` loops 42 re-taps ⟹ `MDBX_PROBLEM` for every reader start; `ENSURE_OBJ(txn->txnid >= cached)` at txn-ro.c:121/284 and txn-basal.c:253 is release-fatal at txn begin/end. Fail-safe (no corruption) but a reader-visible DoS until a writer's rescan heals the cache; read-only-only deployments never heal it. |
| V-032 | cursor-seek.c:676, 844 | low (info) | `cursor_distance_at`'s eof_hard rank adjustment (`iter_ki = ki + 1`) assumes the `ki == nkeys-1` encoding; the SET_RANGE fast-path past-end state (`ki == nkeys`) makes same-page distances over-count by 1 and cross-page distances start at `-1` (size_t wrap in `cdr_t.distance`). `cursor_distribute` absorbs the overshoot (NOTFOUND → `be_poor` + RESULT_TRUE, release-safe), but `ASSERT(tdr.distance > 0)` trips in checking builds and the cursor-split counts skew by one. |
| V-033 | mvcc-readers.c:394-502 | info (liveness) | `mvcc_kick_laggards`: an hsr callback that always returns 0 (declines) never breaks the do-while and never advances the oldest ⟹ `INT_MAX` iterations of a full reader scan + callback under the writer mutex — an effectively unbounded writer stall. Terminates in principle (INT_MAX) and matches the hsr contract for a giving-up (< 0) callback; same family as V-004. |

## 5. New invariants established (registered in the KB as fact:verification:*)
PAIR-N7 (page_split root-split stack bound), PAIR-N8 (page_get ILL dispatch),
PAIR-N9 (front_txnid page-state machine incl. the nested-WRITEMAP prohibition as
the load-bearing gate), PAIR-N10 (cached-oldest single-writer/distrust discipline
and kick-cycle termination), PAIR-N11 (tail-refund reader safety), plus the
unsafe-meta-read-sites fact (where `unsafe_txnid`/`unsafe_sign` raw reads are safe).
