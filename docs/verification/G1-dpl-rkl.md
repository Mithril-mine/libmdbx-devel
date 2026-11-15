# G1 verification report — dpl.{c,h}, rkl.{c,h} (stream A, bottom-up)

Scope: every function of `src/dpl.c` (15), `src/dpl.h` (10), `src/rkl.c` (27), `src/rkl.h` (2). Call sites enumerated in page-ops.c/h, page-get.c, spill.c, txn-basal.c, txn-nested.c, defrag.c, gc-get.c, gc-put.c, env.c, api-opts.c; macro expansions of SEARCH/SORT/RADIXSORT (src/sort.h), CHECKS* gating, option defaults (options.h: MDBX_PNL_ASCENDING=0, MDBX_DPL_PREALLOC_FOR_RADIXSORT=1, MDBX_DPL_CACHE_NPAGES=1 on 64-bit / 0 on 32-bit, MDBX_DEBUG=0 ⇒ MDBX_CHECKING=0 ⇒ all ASSERT compile out in release).

## Lemmas established

- **SORT-search:** lower-bound bsearch — first `it` with `!CMP(*it, item)`; all before `< item`, all from `it` on `≥ item`; length==0 → begin, no logical-range deref (the CMOV do-while reads `*begin` even for length==0 — in DPL that slot is the `items[1]` stub, allocated and initialized by `dpl_setlen` — defined read, result unused).
- **SORT-quicksort / SORT-radix:** see G1-pnl-dml-txl.md.
- **DPL-cap:** the malloc block backing a `dpl_t` provides `≥ 2*detent + dpl_reserve_gap(=18)` `dp_t` slots (size2bytes doubles the request, bytes2size halves; 16 KiB granulate adds slack only; STATIC_ASSERT rules out overflow given MAX_PAGENO). Invariants: `length ≤ detent`; stub writes reach at most index `detent+2 ≤ 2*detent+17`. All DPL index arithmetic stays within this capacity.
- **DPL-order:** entries are pairwise disjoint page ranges, strictly ascending in the `sorted` head; sentinels `items[0].pgno == 0 < every real pgno ≥ NUM_METAS ≤ MAX_PAGENO < items[length+1].pgno == P_INVALID`. Maintained by append/remove/sift; `txn_dpl_check` verifies in audit builds.
- **RKL-INV:** **I1** empty-init `solid_begin=UINT64_MAX, solid_end=0, list_length=0`; **I2** `rkl_empty ⇔ solid_begin > solid_end`; after clear, every reachable state has `solid_begin ≤ solid_end`; **I3** `list[0..list_length)` strictly ascending; **I4** no list element in `[solid_begin-1, solid_end]` (adjacent values absorbed by push/extend, never inserted; `rkl_check` rejects floats/adjacency); **I5** `list_length ≤ list_limit`. All mutators preserve I1–I5 (proofs inline below).

## Assumptions

- **A-DPL-DUP:** callers of `txn_dpl_append` never append a pgno already present and ranges never overlap (release depends on it; enforced in debug by CHECKS2 scan + `txn_dpl_check`). Sites: page_dirty (page-ops.c:759), spill.c:229 (fresh list), defrag.c:468 (old entry removed first at :467).
- **A-DPL-DOM:** dirty `pgno ≥ NUM_METAS`, `pgno+npages ≤ first_unallocated ≤ MAX_PAGENO+1`, `npages ≥ 1`.
- **A-DPL-HOLES:** DPL entries with `ptr==nullptr` exist only transiently inside `nested_merge` (txn-nested.c:206/234/274) and are compacted before `dpl_setlen(dst, dst->sorted)`/any other consumer; `txn_dpl_clear`, `txn_dpl_lru_reduce`, `txn_dpl_check` never observe holes.
- **A-RKL-32:** `rkl_len(rkl) < 2^32` (list_length ≤ txl_max ≈ 2^26; solid span < 2^31 by PAGELIST_LIMIT) — required by `(unsigned)` casts in `rkl_iterator/rkl_turn/rkl_hole`.
- **A-RKL-DOM:** every pushed id satisfies `MIN_TXNID ≤ id < INVALID_TXNID` (push asserts; 0 valid "none" sentinel).
- **A-RKL-PROTO:** consumers of the iterator API follow the protocol: `rkl_turn` returning 0 ends iteration; in reverse, a hole with `begin==1` is terminal (PAIR-C-01).
- **A-NO-SELF-MOVE:** `rkl_destructive_move(src,dst)` never called with `src==dst` (sites: txn-nested.c:640-641, distinct objects).

## Ledger — src/dpl.h

### dpl_setlen (dpl.h:8-22) — PROVED
P1/P2: `len` from callers ≤ detent (all 10+ sites enumerated: spill.c:408, txn-basal.c:53, refund.c:116/138, gc-put.c:349, txn-nested.c:21/308/318, dpl.c init/clear/remove/sift). P3: stub write `items[len+1]` ≤ `detent+1` (DPL-cap); static-const stub E never written through. P5: returns len; overwrites at most the single discarded slot on shrink (caller has disposed of that entry). Shrinking past `sorted` is the caller's duty (all sites keep `sorted == len`).

### txn_dpl_sort (dpl.h:32-41) — PROVED·A
Dispatch `sorted==length → dl` else slowpath; asserts `sorted ≤ length ≤ PAGELIST_LIMIT`, stubs valid. A: callers hold DPL invariants.

### dp_npages (dpl.h:47-54) — PROVED·A
`MDBX_DPL_CACHE_NPAGES=1` (64-bit): cached `npages` with ASSERT consistency for P_LARGE. 32-bit (=0): derefs `dp->ptr` — non-null by A-DPL-HOLES.

### dpl_npages (dpl.h:56-59) — PROVED. `i ≤ dl->length` asserted; index within DPL-cap.

### dpl_endpgno (dpl.h:61-63) — PROVED·A
`uint32 + uint32` no wrap: `pgno+npages ≤ MAX_PAGENO+1` (A-DPL-DOM). For `i==0` reads stub (returns 1, harmless — only used where `1 > pgno ≥ 3` is false).

### txn_dpl_intersect (dpl.h:65-91) — PROVED·A
P1: requires `sorted==length` (tASSERT; sites defrag.c:479, page-ops.c:624 comply). `n = search(pgno) ∈ [1, length+1]`; `items[n] ≥ pgno > items[n-1].pgno`. Completeness of the 2-way test: for `i ≤ n-2`, disjointness gives `end_i ≤ pgno_{i+1} < pgno`, so only `n-1` and `n` can intersect `[pgno, pgno+npages)`. First term: `pgno+npages > items[n].pgno`; second: `endpgno(n-1) > pgno`. P2: no wrap (A-DPL-DOM). CHECKS1 brute-force agrees.

### txn_dpl_exist (dpl.h:93-99) — PROVED·A
Exact match anywhere (linear over unsorted tail ≤ 7 case-by-case; `default` sorts first — verified at txn_dpl_search), else lower-bound index whose `items[i].pgno != pgno` → 0. `i ≥ 1` always.

### txn_dpl_remove (dpl.h:103-105) — PROVED. Passes the removed entry's own page count (see remove_ex).

### txn_dpl_age (dpl.h:111-117) — PROVED·A
P3: reads the 8-byte LRU stamp preceding shadow pages — allocated by `page_shadow_alloc` (size+sizeof(size_t), ptr+8 returned; 8-aligned). Only called for non-WRITEMAP txns where append wrote the stamp. P2: uint32 subtraction wrap intended (monotone stamps; `lru_reduce` halves both).

### txn_dpl_lru_turn (dpl.h:121-126) — PROVED·A
`dirtylru > UINT32_MAX/3` triggers `lru_reduce` before any wrap (non-WRITEMAP only; in WRITEMAP ages never consulted).

## Ledger — src/dpl.c

### dpl_init (dpl.c:6-21) — PROVED
items[0] stub pgno=0; `sorted = dpl_setlen(dl,0)` writes items[1] stub. Requires capacity ≥ 2 slots (DPL-cap).

### dpl_size2bytes (dpl.c:23-36) — PROVED·A
P2: doubling and ceil_powerof2 cannot overflow (STATIC_ASSERT both word sizes); result ≥ payload, detent ≥ requested size. A: caller passes `32 ≤ size ≤ PAGELIST_LIMIT` — see V-014 for the micro-map corner.

### dpl_bytes2size (dpl.c:38-45) — PROVED·A
`bytes ≥ sizeof(dpl_t)+2*32*16` for any reachable reserve ⇒ no negative; ASSERT slack bound holds (granulate/2 = 512 ≤ 1024).

### txn_dpl_free (dpl.c:47-53) — PROVED. clear + free; null-safe.

### txn_dpl_reserve (dpl.c:55-70) — PROVED·A
P2: clamp `min(size, PAGELIST_LIMIT)`; realloc/usable_size ⇒ detent ≥ requested. Shrink path (`detent > 2*wanna`, only from txn_dpl_alloc) safe: `dl->length` pre-zeroed exactly when `tASSERT0` can be live (`#if MDBX_CHECKING > 0`), and `detent_new ≥ wanna > length=0`. P5: nullptr → callers return ENOMEM. Caller txn-nested.c:620-622 only reserves when `sum > detent` (no shrink) and re-reads `txn->wr.dirtylist` afterwards.

### txn_dpl_alloc (dpl.c:72-92) — PROVED·A — FINDING V-014
P1: `wanna = nested ? 32 : min(dp_initial, geo.upper)`; `dp_initial` clamped ≥ 128 (api-opts.c:342-350). Reserve iff list absent or detent out of `[wanna, 2*wanna]`. dpl_init resets fully. **V-014**: for a micro-map (pagesize=256, `size_upper = 1024` bytes ⇒ `geo.upper = 4`), `wanna = 4` violates `ASSERT(size >= CURSOR_STACK_SIZE)` in dpl_size2bytes — reachable only in `MDBX_CHECKING ≥ 1` builds; release safe (16 KiB granulate floor ⇒ detent ≥ 502 ≥ any need). Severity low (debug-only assert). Reproducer sketch: pagesize=256, size_upper=1024 → write txn → assert.

### txn_dpl_sort_slowpath (dpl.c:100-147) — PROVED·A
Three paths: (a) radix (unsorted ≥ 142): L-RADIX + DPL-cap give tmp region `items[length+2 .. 2*length+1]` in-bounds. (b) merge (sorted > unsorted/4+4; with PREALLOC=1 the space disjunct is vacuously true): unsorted tail copied to `tmp = items + 2*length + 1 - unsorted > items+length+1`; backward merge writes exactly `items[1..length]`, consumes all of both runs — count check: writes `length − l_final`, sorted-consumed `sorted − l_final`, radix-consumed `length − sorted = unsorted`, hence `r` ends at `tmp−1` (ASSERTed); write cursor never passes read cursor; stub `items[0]` (pgno 0) can never win `l->pgno > r->pgno` since real pgnos ≥ 3 ⇒ never merged as data; loop exit `--w > l` leaves the remaining sorted head in place. (c) plain `dp_sort(dl->items+1, dl->items+dl->length+1)` — indices 1..length, stubs untouched. All paths end `sorted = length`; final ASSERTs recheck sentinels; CHECKS1 verifies strict order incl. `items[length] < P_INVALID`. P4: all loops bounded. cmov/non-cmov merge variants equivalent.

### txn_dpl_search (dpl.c:154-196) — PROVED·A
Case-split complete: `length−sorted` = 0 → pure bsearch; 1..7 → linear scan of exactly the unsorted tail then bsearch over sorted head; `default` (>7) → sort whole first. Result: index of exact match anywhere, or lower bound in [1, sorted+1]. `length==0` → 1 (stub; callers compare pgno ≠ query). CHECKS2 head-scan guard `--ptr > items` safe for sorted==0. Slowpath does not reallocate, local `dl` stays valid, `dl->sorted` re-read after.

### debug_txn_dpl_find (dpl.c:198-217) — PROVED·A
Tail scan `i ∈ (sorted, length]`, then guarded bsearch, exact-match returns. Null-dl branch asserts WRITEMAP context.

### txn_dpl_remove_ex (dpl.c:219-231) — PROVED·A
`1 ≤ i ≤ length` asserted (sites: page-ops.h:131, page-ops.c:739, defrag.c:467). `sorted -= (sorted >= i)` correct for head/tail removal; memmove moves `items[i+1..length_old+1] → items[i..length_new+1]`, relocating the stub to the new `length+1`; all indices ≤ `detent+1` (DPL-cap). `pages_including_loose -= npages`: no underflow since callers pass the entry's own count (A-DPL-DUP/DOM; defrag passes `arc->npages` equal to it).

### txn_dpl_append (dpl.c:233-342) — PROVED·A
P1: growth at `length==detent` with `detent ≥ PAGELIST_LIMIT → MDBX_TXN_FULL`; doubling (below `MDBX_PNL_INITIAL*42`) or +50%; reserve result checked. Pivot heuristic: `pivot = length−42`; branch `pivot ≤ sorted && pivot_pgno < dp.pgno` selects shift-insert into the sorted head only when the 42-back sentinel element (or stub 0 when length ≤ 42; cmov variant indexes `max(length−42, 0)` — equivalent at length==42) is inside the sorted head and smaller than dp.pgno — correct for cmov and non-cmov forms; else append to unsorted tail. P3: stub copy `i[2]=i[1]` writes index `length+2 ≤ detent+1`; final write `i[1]` index `length_new ≤ detent`; all ≤ `2*detent+17` (DPL-cap). Tail shift moves `items[sorted_old+1..length_old]` up one, head shift moves down while `i->pgno > pgno` — terminates at `items[0]` at worst (`pgno ≥ NUM_METAS > 0`); assert `i > items` never violated since the body requires `items[i].pgno > pgno ≥ 3`. Post-insert `i->pgno < dp.pgno` (equality = duplicate, excluded by A-DPL-DUP; debug CHECKS2 scan also rejects). 128-bit shift copy: `dp_t` = 16 B, `items` 16-aligned. P5: `sorted ≤ length` preserved; `pages_including_loose += npages` before commit; callers decrement dirtyroom. LRU stamp written only for non-WRITEMAP shadow-backed pages (all append sites comply — defrag auxbuf path does NOT append).

### txn_dpl_check (dpl.c:344-423) — PROVED·A
Dirtyroom identity; hole-skip valid per A-DPL-HOLES; age bound matches lru_turn threshold; `P_LOOSE ∨ is_modifiable`; `first_unallocated ≥ pgno+num`; sorted-head non-overlap `items[i+1].pgno ≥ pgno+num` (i+1 ≤ sorted); repnl absence of pgno and of the whole span; loose/pages totals; retired∩dpl = ∅ via debug_txn_dpl_find. Every tASSERT paired with a runtime `return false` for audit builds. P5: `true` iff all invariants hold.

### txn_dpl_lru_reduce (dpl.c:427-439) — PROVED·A
Halves dirtylru and every stamp, then recurses to parent. Non-WRITEMAP only; stamps exist for all entries incl. parents'. P4: dirtylru strictly decreases ⇒ terminates.

### txn_dpl_sift (dpl.c:441-505) — PROVED·A
Scan is an ascending-value merge between descending PNL (ASCENDING=0: `step=−1, begin=size, end=0`, `pl[size] ≤ pl[1]` asserted) and the fully sorted DPL (txn_dpl_sort at :446 before search — pure-bsearch path). Scan loop advances `r` or `i` monotonically; exit via loop condition or `i == end` with no prior match ⇒ list untouched. Update loop: `w=r`; each match removes via remove_dl (decrements pages_including_loose once, releases shadow per sync-mode guard, `++r`); next_i bounds-checked, on `i==end` compacts the remainder; otherwise keep when dp<pl. Exit accounting: removed = r − w; `dirtyroom += r−w`; `setlen(w−1)` — kept count; stub write index `w ≤ length+1`; `sorted == length` after. P4: r/i monotone, bounded. The inner `ASSERT(i != end)` dischargeable by construction.

### txn_dpl_clear (dpl.c:507-518) — PROVED·A
Shadow release only for plain non-WRITEMAP (holes impossible per A-DPL-HOLES). setlen(0) re-anchors stub. Stale `pages_including_loose` acceptable: every subsequent use goes through dpl_init or txn_dpl_free.

## Ledger — src/rkl.h

### rkl_empty (rkl.h:43-45) — PROVED
`begin > end` ⇔ empty per I1/I2 (pop/extend keep `begin ≤ end` in live states; `begin==end` counts as empty consistently with solid_empty).

### rkl_edge (rkl.h:50-53) — PROVED. Pure dispatch to lowest/highest.

## Ledger — src/rkl.c

### rkl_size2bytes (rkl.c:6-11) — PROVED·A
`wanna ∈ (12, txl_max]` from all callers (resize caps at txl_max; the ≤12 branch bypasses); `txl_max*2` bound asserted; ceil−overhead ≥ payload.

### rkl_bytes2size (rkl.c:13-17) — PROVED
`bytes/8 ≤ txl_max+32 ≤ txl_max*2` (granulate 256 B). Cast `(unsigned)` safe (STATIC_ASSERT `txl_max < INT_MAX/8`: 67108862 < 268435455).

### rkl_init (rkl.c:19-23) — PROVED. limit=12, list=inplace, clear (I1).
### rkl_clear (rkl.c:25-29) — PROVED. I1 reset; heap buffer intentionally retained.
### rkl_clear_and_shrink (rkl.c:31-38) — PROVED. Frees heap, restores inplace (I1+I5).
### rkl_destroy (rkl.c:40-45) — PROVED. `osal_free(nullptr)`-safe on double destroy; sets list=nullptr.
### solid_empty (rkl.c:47) — PROVED. `!(begin < end)` ⇔ empty (I2).

### rkl_destructive_move (rkl.c:53-69) — PROVED·A
Heap buffer ownership transferred (src re-inited without freeing — correct move); inplace case memcpy(12×8 B) with limit set to 12. dst's old heap freed before overwrite. P5: dst inherits I1–I5; src fresh-empty. A-NO-SELF-MOVE (self-move would leak; not reachable).

### rkl_resize (rkl.c:71-113) — PROVED·A
P1: `wanna > txl_max → TXN_FULL`; `wanna < list_length → MDBX_PROBLEM` (deviant precondition guard, reachable only in probe builds per DEV_ASSERT comment). Inplace branch (`wanna ≤ 12`): unreachable with a heap list (reserve requires wanna > limit ≥ 13; copy requires `12 < src->list_length ≤ 12` — impossible) — defensive dead code (PAIR-N-01), its ASSERTs catch misuse. Heap branch (`wanna != limit`): malloc/realloc, ENOMEM preserves old buffer; usable_size ⇒ limit ≥ wanna; inplace→heap memcpy 96 B ≤ capacity ≥ 104 B. P5: all error paths leave the rkl valid.

### rkl_reserve (rkl.c:115-117) — PROVED·A. Only grows (`wanna > limit ≥ list_length` ⇒ resize precondition satisfied).

### rkl_copy (rkl.c:119-134) — PROVED·A
Fresh-init dst; grows to `src->list_limit ≥ src->list_length` when needed; memcpy exactly `list_length` elements; solid fields copied only when nonempty. P5: error leaves dst initialized-empty.

### rkl_len (rkl.c:136-138) — PROVED·A
`(end−begin) + list_length`, non-negative by I2; < 2^32 by A-RKL-32.

### rkl_contain (rkl.c:140-156) — PROVED·A
Solid interval test; list bsearch lower bound (SORT-search), exact match only. Duplicates impossible (I3/I4).

### rkl_find (rkl.c:158-181) — PROVED·A
Position mapping verified for all three zones: id inside solid → `pos = solid_offset + (id−begin)` exact, true; id ≥ end → lower bound `it ≥ solid_offset` (everything below solid_offset is < solid_begin ≤ id), `pos = solid_len + it`; id < begin → `pos = it ≤ solid_offset`. Casts bounded by A-RKL-32. Empty rkl → pos 0, false.

### list_remove_first (rkl.c:183-195) — PROVED
Shifts `list[1..old_length)` → `list[0..old_length−1)`; max read index in-bounds; `--list_length` truthiness handles the 1-element case.

### after_cut (rkl.c:197-203) — PROVED
Re-anchors I1 when drained; the degenerate state already equals I1 — no-op, consistent.

### extend_solid (rkl.c:205-240) — PROVED·A (probes traced — see V-015, PAIR-C-04)
Head walk: `f[-1] ≥ solid_begin−1 && f[-1] < id` (lower-bound left neighbors) — for the empty-solid call (`solid_begin == id`) forces `f[-1] == solid_begin−1` exactly, so `*f == solid_begin` after decrement always holds; for adjacent-below/above calls (253/257 — `known_continuous` is constant false, so extend runs only for exact adjacency) the walk cannot start because I4 excludes `[solid_begin−1, solid_end]`. Hence the head-gap probe is **unreachable** in valid states (V-015). Tail walk: absorbs the exact ascending run `solid_end, solid_end+1, …`; `*t < solid_end` fires only when `*t == id` from the empty-solid call — precisely the duplicate-in-list detector (`MDBX_RESULT_TRUE`, correct semantics). Removal `f < t` shifts out the absorbed run; list stays sorted, I4 holds for the new solid. Underflow: `solid_begin−1 ≥ 0` since id ≥ MIN_TXNID. P5: solid assigned; rkl_check asserted.

### rkl_push (rkl.c:242-392) — PROVED·A — special-attention items verified
P1 case-split complete: empty-solid → extend; below-solid (adjacent → extend; else fall through); above-solid (adjacent ⇒ extend; else fall through); inside-solid → RESULT_TRUE (duplicate). Duplicate detection complete across paths: inside-solid direct; in-list with empty/adjacent-below solid → extend_solid probe; in-list otherwise → insertion-loop rollback (295-307). **Rollback (documented historical off-by-one) verified sound**: after `k` right-shifts, the loop `while (i < list_length) list[i] = list[i+1]` starting at `i=k` restores every position `k..L−1` to its pre-push value (position L keeps a stale copy beyond `list_length`, never read); no-shift case skips the loop; structure unchanged, `rkl_check` asserted. **Couple-swap (263-273) verified sound**: the new list element `s` (old solid_begin) can never land adjacent to the new solid `[couple, couple+2)` (dangerous cases `s ∈ {id−1, id, id+1}` excluded by I4 plus earlier absorption/return); no uint64 wrap (`couple+2 ≤ UINT64_MAX` since list elements < INVALID_TXNID). **Grow (275-285)**: clamps keep `list_length+42 ≤ x2 ≤ txl_max`; forced `x2 = list_length+42 > txl_max` → resize TXN_FULL → caller-visible failure, structure intact. **Insertion (287-313)**: index bounds `i < list_limit`; backward shift loop. **Swap-vs-insert cost model (317-390)**: run detection walks exact consecutive neighbors; executed only when `run > 3` and `run > old_solid_len`; at that point solid is provably nonempty (empty-solid returns at :249), so `old_solid_len ≥ 1` — the `(size_t)(0 − UINT64_MAX) = 1` wrap for an empty solid can never feed the swap. `old_solid_pos` = index of first element > solid_end (I4 ⇒ first ≥ solid_end+1). Cost formula matches the moves actually performed in both layout branches: shift-down branch moves `from−pos` (descending copy, reads never overwritten) plus tail `L − to`; shift-up branch's do-while moves exactly `old_solid_pos − R − from` and the tail loop `L − old_solid_pos`, summing to `L − to`; the `to == L` edge writes only the inserted solid over the removed run's first slot with `list_length` shrunk past the stale remainder. Resulting list sorted, disjoint from the new solid; I4 preserved (left walk guarantees `list[from−1] < new_solid_begin − 1`). `list_length = L − R + S > 0` (R > S), cast safe. All loops terminate. MDBX_DEBUG>0 forces the swap for coverage — correctness-neutral.

### rkl_pop (rkl.c:394-410) — PROVED·A
Max/min selection consistent with highest/lowest (I4 ⇒ list entirely above or below solid, two-way comparison exhaustive); list guard prevents `[-1]`; solid pop post/pre-increment correct; both-empty → 0 (callers check: gc-put.c:494 loops on `!rkl_empty`, gc-put.c:957 asserts ≥ MIN_TXNID, defrag.c:225 compares). after_cut re-anchors.

### rkl_lowest / rkl_highest (rkl.c:412-416 / 418-423) — PROVED·A
Exhaustive two-way comparisons; `solid_end − 1 ≥ MIN_TXNID` when solid nonempty (no underflow); INVALID_TXNID / 0 sentinels.

### rkl_merge (rkl.c:425-443) — PROVED·A
Reverse-order push of list elements, ascending push of solid span; duplicate handling per `ignore_duplicates`; error aborts with dst partially updated, src untouched. `++id` cannot wrap (exits at `id == solid_end ≤ UINT64_MAX`). P4 bounded.

### rkl_destructive_merge (rkl.c:445-449) — PROVED·A (N/A in-tree)
No in-tree callers. Destroys src even on error — content loss on failure by design; hazard only if reused (PAIR-C-03).

### rkl_iterator (rkl.c:451-461) — PROVED·A
`pos = reverse ? len : 0` (A-RKL-32); `solid_offset` = lower bound of solid_begin = first element > solid_end (I4), `≤ list_length`; ASSERT discharges.

### rkl_turn (rkl.c:463-484) — PROVED·A
Underflow-safe (`pos = iter->pos − reverse` as size_t; `0−1` → huge → caught by `pos ≥ len` → 0). All three zone mappings in-bounds given `pos < len` and `len = list_length + solid_len`. `iter->pos ≤ len` maintained. List-empty case asserts `pos < solid_len` (== len). Callers treat 0 as depletion (gc-put.c:1366-1370).

### rkl_left (rkl.c:486-489) — PROVED. `pos ≤ len` asserted; no underflow.

### rkl_hole (rkl.c:509-645) — PROVED·A — contract caveat PAIR-C-01
Reverse walk emits gaps strictly decreasing: above-highest, between consecutive list elements, solid↔above (via boundary case `next == solid_offset+solid_len−1`, reached only with reverse — forward cannot hit it since `next=pos+1 > solid_offset+solid_len−1` whenever `pos ≥ solid_offset+solid_len`), below-solid↔solid, before-lowest. All index arithmetic underflow-safe (`next = pos−1` at pos 0 → SIZE_MAX caught by `next ≥ len` in every branch before deref; inside-solid reverse likewise for `solid_offset==0`). `here` sentinel MAX_TXNID for the step-back-from-end entry produces the correct terminal above-gap. Boundary-gap asserts `hole.begin < hole.end` justified by I4 — PAIR-C-02. The final pure-solid block: `reverse && pos < len` always true there except the pos==len fall-through, which correctly returns `[solid_end, MAX_TXNID)`. **PAIR-C-01**: in reverse mode the terminal below-hole is re-emitted on repeated calls (`pos` pinned to 0); forward likewise — the API is single-pass by contract; the sole in-tree consumer (gc_search_holes, gc-put.c:868-920) terminates via `MIN_TXNID >= begin → break` and the `begin >= end → turn` discipline; empty rkl → single full hole. Forward mode has no in-tree callers (tests only) but is symmetric and verified.

### rkl_check (rkl.c:647-691) — PROVED·A
limit/inplace consistency; empty ⇔ (len 0 ∧ solid_empty); strict order scan; float detection: precondition `solid_begin−1 ≤ list[last] && solid_end ≥ list[0]` (no underflow: guarded by `!solid_empty`), then bsearch-based check requires every element < lower_bound ≤ solid_begin−1 and every element from it on ≥ solid_end+1 — exactly I4 given the precondition skips entirely-below/above cases. `it ∈ [list, end]` always (SORT-search) so the range probe is defensive-dead. The check *rejects* adjacency `solid_begin−1`/`solid_end` — consistent with I4 as maintained by the mutators (adjacent values always absorbed, never stored).

## Special-attention trace: MDBX_RESULT_TRUE from extend_solid probes — caller handling

`MPROBE_WATCH` compiles to a no-op outside `MDBX_PROBES` builds; in probe builds it records and execution continues to `return MDBX_RESULT_TRUE` (= −1, excluded from MDBX_IS_ERROR — soft result). Caller handling enumerated:

| Caller | Handling |
|---|---|
| rkl_merge (rkl.c:430/438) | propagates unless `ignore_duplicates` |
| defrag.c:168/241 | `break` with rc = RESULT_TRUE → benign loop stop |
| gc-get.c:1272 | `goto fail` → hard internal error (MDBX_TXN_ERROR unless ALLOC_UNIMPORTANT/RESERVE) |
| gc-put.c:494 (gc_remove_rkl NOTFOUND path) | WARNING + continue — explicitly tolerated |
| gc-put.c:515 | returned; upstream gates on MDBX_IS_ERROR ⇒ soft |
| gc-put.c:537-543 (gc_push_sequel) | converted to MDBX_PROBLEM with ERROR log — strict |
| gc-put.c:968 (gc_reserve4return) | returned to caller |
| gc-put.c:1216 (sequel merge) | converted to MDBX_PROBLEM — strict |

Reachability (proved in the extend_solid row): the **head-gap probe is unreachable** in any state satisfying I4 — dead defensive instrumentation (V-015). The **tail-gap probe is reachable only as duplicate detection** — exactly the documented "already present" semantic. No caller can observe a *gap* result in a correct system; the strict handlers double as inconsistency detectors.

## Findings summary

| V-id | file:line | severity | one-line |
|---|---|---|---|
| V-014 | dpl.c:24 + :59/:75-78/:84 | low (debug-only) | `wanna = min(dp_initial, geo.upper)` can be < CURSOR_STACK_SIZE for micro-maps (geo.upper ≥ 4 pages at 256 B pages), firing `ASSERT(size >= CURSOR_STACK_SIZE)` in MDBX_CHECKING≥1 builds; release safe. Reproducer: pagesize=256, size_upper=1024 |
| V-015 | rkl.c:215-218 | info | `rkl_extend_solid_gap_head` probe unreachable under I4 — dead defensive code |
| PAIR-C-01 | rkl.c:615-627, 631-644 | info (contract) | rkl_hole re-emits its terminal hole on repeated calls; single-pass consumer contract honored by gc_search_holes; must be respected by new consumers |
| PAIR-C-02 | rkl.c:554, 577 | info | `ASSERT(hole.begin < hole.end)` at solid-boundary gaps relies on I4; debug-only trap if a future mutator breaks I4 |
| PAIR-C-03 | rkl.c:445-449 | info | rkl_destructive_merge destroys src even on error; no in-tree callers — hazard only if reused |
| PAIR-N-01 | rkl.c:86-97 | info | rkl_resize shrink-to-inplace branch unreachable from in-tree callers; defensive only |

No REFUTED verdicts: every counterexample attempt (rollback restoration, swap layouts, merge cursor accounting, sift compaction accounting, radix tmp placement, pivot index selection, hole-walk underflow) discharged by the invariants above.

## Method note / residual scope

SEARCH_IMPL/SORT_IMPL/RADIXSORT_IMPL, page_shadow_alloc stamp layout, nested_merge hole lifecycle, dp_initial/geometry clamping verified at lemma level for these files, not re-proven line-by-line; gc_search_holes outer loop termination out of scope (only the rkl_hole protocol it consumes covered). Files verified read-only; no modifications.
