# G1 verification report — pnl.{c,h}, dml.{c,h}, txl.{c,h} (stream A, bottom-up)

Scope: `src/pnl.h`, `pnl.c`, `dml.h`, `dml.c`, `txl.h`, `txl.c` (audit tree byte-identical to master). Caller contexts enumerated tree-wide (defrag.c, gc-put.c, gc-get.c, spill.c, page-ops.c, txn-nested.c, txn-basal.c, chk-walk.c, api-gc.c, tests/ut).

## src/pnl.h

### pnl_alloclen / pnl_size (pnl.h:47-51) — PROVED
P3: reads pnl[-1] / pnl[0], both inside the malloc'd raw buffer (raw[0]=alloclen, raw[1]=len, entries raw[2..]). P2: unsigned, no arithmetic.

### pnl_setsize (pnl.h:53-56) — PROVED·A
P1/P2: cast `(pgno_t)len` — domain proof: every writer passes len ≤ alloclen and alloclen ≤ bytes2size(PAGELIST_LIMIT-request) < INT_MAX (Lemma PNL-slack; boundary case folded into V-008). In release the pgno_t (uint32) cast is lossless since len < 2^32.
ASSUMPTIONS: len < 2^32; holds under PNL-slack/PNL-capacity and the reserve PAGELIST_LIMIT paranoia gate.

### pnl_clear (pnl.h:58) — PROVED
### MDBX_PNL_* macros (pnl.h:18-39) — PROVED
P1: with MDBX_PNL_ASCENDING==0 the ASCENDING half is compile-time dead; descending defs: EDGE = pl+len (last entry, in-bounds when len ≥ 1), MOST = pnl[1], LEAST = pnl[len]; CONTIGUOUS(prev,next,span) = prev−next == span — correct for a descending run. P3: LAST/EDGE index len ≤ alloclen (PNL-capacity).

### pnl_bytes2size (pnl.h:63-71) — PROVED, FINDING V-005
P2: inverse of size2bytes: S = bytes/4; alloclen = (S−3)>>1. Round-trip: for request w, bytes ≥ 8w+12 ⇒ S ≥ 2w+3 ⇒ alloclen ≥ w ✓. P1: the ASSERT `size <= PAGELIST_LIMIT + 65536` is WRONG for near-limit allocations: the input S is the pre-halving slot count ≈ 2·w + ≤1024 slack; for w = PL (64-bit: 2^31−1) S ≈ 4294968316 > PL+65536 → spurious abort in checking builds (reproducer: DB with ~2^31 pages, list grown to PL → any re-alloc of a ≈PL-size list). Release arithmetic correct. Same defective upper bound in dml_bytes2size. Severity low (debug-only false alarm).

### pnl_size2bytes (pnl.h:73-87) — PROVED
P1: ASSERT(wanna > 0 && ≤ PL) — callers comply (reserve clamps to PL; alloc callers pass ≥1; clone of a slack-oversized alloclen is the V-008 boundary). P2: doubling then bytes2size halving cancel ⇒ alloclen ≥ wanna; STATIC_ASSERT rules out overflow on 64/32-bit. P5: pure.

### pnl_need (pnl.h:97-106) — PROVED (note V-008)
P1: asserts len ≤ PL, alloclen ≥ len, more ≤ PL; early-success iff alloclen ≥ len+more ⇒ writes pnl[1..len+more] in-bounds. Else reserve(len+more); reserve rejects > PL — except the early-success path when a previous allocation left alloclen > PL (granulation slack, reachable only after a ≈PL-size request): then len may creep to alloclen > PL by ≤ ~510 entries and pnl_check_lite would reject a self-built list (V-008, low). P5: failure returns reserve's rc, list untouched.

### pnl_append_prereserved (pnl.h:108-116) — PROVED
P1: precondition size < alloclen (all callers go through pnl_need/pnl_reserve first — verified each). P3: writes pnl[size+1] ≤ alloclen ✓. CHECKS2 dup scan: indices size..1 in-bounds, O(n), assertion-only. P5: len increments by exactly 1, entry stored.

### pnl_append (pnl.h:118-123) — PROVED
Composes pnl_need + prereserved; rc propagated; on failure no mutation.

### pnl_check_allocated / pnl_check_full_allocated (pnl.h:153-159) — PROVED (note V-012)
P1: null ⇒ true (documented tolerance; V-012: makes it useless as a null-guard before deref); else alloclen ≥ len && (lite|full).

### pnl_sort (pnl.h:161-165) — PROVED
Sorts, then ASSERT(check_lite) — assertion-only postcondition.

### pnl_search (pnl.h:167-183) — PROVED·A, FINDING V-012
P1: ASSERT(check_allocated(pnl, limit)) — null-tolerant immediately before deref (V-012, misleading guard; every in-tree caller passes a live list). ASSERT(pgno < limit) — callers pass MAX_PAGENO with valid pgno. P3: `VALGRIND_MAKE_MEM_DEFINED(MDBX_PNL_END(pnl))` — the one-past-data entry pnl[len+1] is inside the raw allocation (PNL-slack) but uninitialized; cmov bsearch may read it without using it (SORT-search lemma: result unaffected). P5: returns it−begin+1 ∈ [1, len+1]; callers guard n ≤ len before indexing (search_exact/contains) or tolerate len+1 (sift/cut_range loops).
ASSUMPTIONS: non-null pnl (in-tree holds).

### pnl_search_exact / pnl_contains (pnl.h:185-193) — PROVED
P1/P3: `n <= pnl_size(pnl) && pnl[n] == pgno` — short-circuit excludes the n == len+1 overread position.

### pnl_contains_span (pnl.h:195-203) — PROVED·A, FINDING V-009
P1: ASCENDING arm dead. Descending: `n >= span && pnl[n] == pgno && CONTIGUOUS(pnl[n-span+1], pnl[n], span-1)`. P3: n−span+1 ≥ 1; n ≤ len or n == len+1 — the short-circuit does NOT exclude n == len+1 ⇒ reads pnl[len+1]: memory-safe (PNL-slack) but a stale-garbage read can yield a false "true". In-tree the only use is ASSERT inside pnl_get_best_sequence (pnl.c:355) where pgno was just read from the list ⇒ benign. No other callers.
ASSUMPTIONS: callers guarantee pgno present (in-tree: true).

### pnl_scan_span (pnl.h:211-218) — PROVED
P1: ASSERT(0 < from ≤ len). P3/P4: loop guard `from + span <= len` precedes the read ⇒ bounded; span strictly increases ⇒ terminates. Callers (defrag.c:115, api-gc.c:75, chk-walk.c:781) drive i from 1; chk-walk explicitly skips clamped (crafted-length) records — caller-side guard confirmed.

## src/pnl.c

### pnl_alloc (pnl.c:6-19) — PROVED
P2/P3: bytes ≥ 8·size+12; writes raw[0] (alloclen) and raw[1] (len=0) — buffer ≥ 7 slots even for size 0 (granulation floor) ✓. ASSERT(alloclen ≥ size) via PNL-slack. P5: nullptr on OOM, else valid empty list. No in-tree caller passes size 0.

### pnl_free (pnl.c:21-24) — PROVED. Null-safe.

### pnl_clone (pnl.c:26-31) — PROVED (boundary → V-008)
P3: memcpy(pl, src, (len+1)·4) preserves pl[-1]=alloclen; copies src[0]=len. Capacity: alloc(alloclen(src)) ⇒ clone alloclen ≥ alloclen(src) ≥ len. Boundary: alloclen(src) > PL (V-008) would trip size2bytes' ASSERT in checking builds; release fine.

### pnl_shrink (pnl.c:33-50) — PROVED
P1: leading ASSERT verifies the size2bytes/bytes2size round-trip on MDBX_PNL_INITIAL numerically (computed 1018 → 8176 → 1020 ∈ [1018, 1527) ✓; 32-bit: 1021 ✓). Threshold: alloclen > INITIAL·8 − 16 triggers realloc → new alloclen ≈ 2041 < old (true shrink) and ≥ 2·INITIAL ≥ len(=0) ✓. P5: setsize(0) applied BEFORE realloc; on realloc failure *ppnl unchanged and valid; *ppnl updated only on success ✓.

### pnl_reserve (pnl.c:52-81) — PROVED
P1/P5: null → alloc; `allocated >= wanna` early-out; `wanna > PL` → TXN_FULL — the gate maintaining the capacity invariant. P2: `wanna+wanna−allocated` reached only when allocated < wanna ⇒ no underflow; new size = min(2·wanna−allocated, PL) ≥ wanna in both arms ⇒ ASSERT(*pnl ≥ wanna) proven. P3: realloc raw−1; header rewritten from usable_size; *ppnl moved only on success; on ENOMEM old allocation untouched ✓. Overflow: wanna ≤ 2·PL fits size_t ✓.

### pnl_append_stepped (pnl.c:83-112) — PROVED·A
P1: ASSERT(n > 0); n==1 → prereserved; n>1 → bulk append, NO dup/order-vs-existing check (assumption A1). P3: after pnl_need, len+n ≤ alloclen; descending arm w = len+n → writes pnl[w] for w = len+n .. len+1, exactly n slots ✓; setsize(w) BEFORE loop — loop uses local w. P4: `--n` + `w--` terminate. P2: `pgno += step` — for step=2 base = pgno<<1 with pgno < first_unallocated ≤ MAX_PAGENO ⇒ no wrap; for step=1 caller ensures pgno+n−1 < 2^32 (holds: pages < first_unallocated ≤ MAX_PAGENO). P5: appended block internally descending (values descend as index descends) — matches layout; global order requires A1.
ASSUMPTIONS: A1 (appended span disjoint from and below the current tail; global sort re-established at consumption points, e.g. gc-put.c:382/461).

### spill_append_span (pnl.c:114-116) — PROVED·A
`pgno << 1`: fits, no UB; spill.c:70 caller passes dirty pages with pgno+npages ≤ first_unallocated. Odd-tag scheme consistent with spill.c:47-52. A1.

### pnl_append_span (pnl.c:118-120) — PROVED·A
Callers: defrag.c:649/1031/1116, page-ops.c:542; global sort at consumption. A1.

### pnl_append_pnl (pnl.c:122-133) — PROVED
P1: empty src ⇒ no-op. P3: reserve first ⇒ memcpy writes dst[len_dst+1 .. len_dst+src_len] ≤ alloclen; dst realloc cannot invalidate src (all in-tree calls use distinct buffers). P5: setsize(total) after copy; error propagates with dst untouched. ORDER: plain append preserves global order only if all dst ≥ all src; in-tree: defrag_load_gc sorts afterwards (defrag.c:182), defrag.c:759-761 re-sorts, retired sorted at GC-write (gc-put.c:382/461) ⇒ caller-side contract satisfied.

### pnl_insert_span (pnl.c:135-151) — PROVED·A
P1: ASSERT(n > 0); pnl_need first ⇒ len+n ≤ alloclen. P3/P4: shift loop reads/writes indices ≥ 1, r strictly decreases, w ≥ r always ⇒ in-bounds; terminates. Fill loop: exactly n writes at r+1..r+n (traced example verified: [50,40,30,20] insert 25 n=2 → [50,40,26,25,30,20] ✓); fill = pgno..pgno+n−1 ascending as index descends ✓. P5: setsize(len+n) before loops, loops only move/overwrite within [1..len+n]. ORDER/DUP caveat: strictly sorted iff no existing entry lies in (pgno, pgno+n−1] (existing == pgno gets shifted then duplicated ⇒ silent duplicate; no runtime check). In-tree callers (page-ops.c:657/734, txn-nested.c:84/399) insert pages provably absent from repnl (a page cannot be simultaneously dirty/loose/retired and reclaimed) ⇒ holds; latent hazard if a future caller violates it.
ASSUMPTIONS: A1 (strict form: span [pgno, pgno+n−1] disjoint from current entries).

### pnl_check_lite (pnl.c:153-166) — PROVED (note V-008)
P1: len==0 → true; len > PL → false (the check V-008's slack would trip); len>1 && LEAST ≥ MOST → false (O(1) by design, documented); LEAST < MIN_PAGENO=3 → false; MOST ≥ limit → false. For len==1 both range checks apply ✓. P3: reads pnl[0], pnl[1], pnl[len] ✓. Not gated by MDBX_DISABLE_VALIDATION; gating happens at callers (gc-get.c:937 lite-vs-full by MDBX_VALIDATION — V-010/V-022).

### pnl_check_full (pnl.c:168-182) — PROVED
lite + strict-descending adjacent scan: do-while runs exactly len−1 times; ORDERED(prev, cur) = prev > cur rejects disorder and duplicates ✓. MDBX_DISABLE_VALIDATION does NOT gate this function; defrag.c:191 always runs it on repnl after GC load, gc-get.c:937 gates by MDBX_VALIDATION.

### pnl_merge_inner (pnl.c:191-223) — PROVED·A
Backward merge of two strictly-descending runs, sentinel dst[0]. P3/P1: cmov arm — exactly one of src_a/src_b decremented per iteration; reaching dst[0]=P_INVALID makes ORDERED(src_b, P_INVALID) false forever ⇒ reads bounded: src_a ≥ dst+0; iterations ≤ total ⇒ lowest write index ≥ 1, sentinel preserved. Non-cmov arm equivalent. Termination: src_b strictly decreases once per outer iteration ✓. Duplicates flag: `duplicates |= !MDBX_DISABLE_VALIDATION && *src_a == *src_b` — catches exactly cross-list equal pairs; with strictly-sorted inputs every duplicate pair in the output is an a==b pair at the comparison point. With MDBX_DISABLE_VALIDATION==1 the flag is constant false (V-010).
ASSUMPTIONS: A2 (both inputs strictly sorted; equal neighbors inside ONE input are not detected — e.g. dst=[7], src=[5,5] → [7,5,5] with flag unset).

### pnl_merge (pnl.c:230-255) — PROVED·A
P1: src_len==0 ⇒ dst_len unchanged ✓. Case split: (a) dst_len==0 or LAST(dst) > FIRST(src): memcpy after dst ✓; (b) LAST(src) > FIRST(dst): memmove up (overlap-safe) + memcpy front ✓; (c) merge_inner (label also reached by goto when total < 21/12 — the label sits AFTER `dst[0] = P_INVALID`, sentinel set on both entries ✓). P3: capacity ASSERT — proved at every call site: gc-get.c:1174 pnl_need ⇒ ✓; txn-nested.c:324 preserved reservation; gc-put.c:322 overlap case: pnl_need(2·loose+2) ⇒ merge write range [1..len+loose] strictly below the embedded src window [alloclen−loose, alloclen−1] (algebra from the need amount) — promoted to proved fact (PAIR-N2). P2: total fits ✓. P5: setsize(total − duplicates); "one less regardless of pair count" is exactly the gc-get.c:1189 contract ✓.
ASSUMPTIONS: A2 (src strictly sorted; GC path guarantees it only under MDBX_VALIDATION — V-010/V-022).

### pnl_intersect (pnl.c:262-282) — PROVED (dead in production, V-011)
Two-pointer scan on descending lists; guards precede reads; terminates (each iteration advances ia or ib). No production callers — only tests/ut/api/details_pnl.c (V-011).

### pgno_radixsort wiring + pnl_sort_nochk (pnl.c:284-297) — PROVED
RADIXSORT_IMPL(PREALLOC=1, END_GAP=0): ≤2 passes for 32-bit keys; exclusive-prefix counters sum to length ⇒ tmp[0..len−1] fully written before b-distribution reads it; stable LSD ⇒ total descending order; EXTRACT_KEY domain: pgno ≤ 2^32−4 ⇒ key ≥ 0 no underflow. In-buffer tmp = begin+length = &pnl[1+len], writes entries len+1..2len ⇒ raw indices ≤ 2·len+1 ≤ S−1 by PNL-slack (this is what MDBX_PNL_PREALLOC_FOR_RADIXSORT pays for). pnl_sort_nochk: len < 142 → quicksort; else radix (fallback quicksort branch dead with PREALLOC=1). Quicksort internals per SORT-quicksort lemma.

### pgno_bsearch wiring + pnl_search_nochk (pnl.c:299-311) — PROVED
SORT-search lemma: it+length ≤ end through both variants; cmov one-past read is in-allocation and result-neutral; result ∈ [begin, end]; returns it−begin+1 ∈ [1, len+1].

### pnl_maxspan (pnl.c:313-326) — PROVED
P1: len ≤ 1 ⇒ len. Scan: invariant scan_idx + span = consumed+1; last read index = len ✓; CONTIGUOUS = run-length measure; scan advances only on non-contiguous ✓. P4 ✓. Callers: gc-put.c:147, chk-walk.c:779 (validated, non-clamped record — caller guard at 771-776 prevents crafted-length overread, explicitly commented).

### pnl_get_best_sequence (pnl.c:328-361) — PROVED·A, FINDING V-006
P1: ASCENDING arm dead. Outer for: condition `i >= seq && pnl[i] <= defrag_detent − seq` — the subtraction wraps if detent < seq; PROVEN impossible at all three in-tree call sites (detent≥seq lemma), but unguarded in-function ⇒ V-006 latent. Inner while: read at i−span==0 hits pnl[0] (in-allocation, harmless); second conjunct blocks the increment ⇒ span ≤ i, reads ≥ index 0 only ✓. span ≥ seq branch: extra = span−seq ≥ 0; early break on exact fit ✓. P3: best_pos = i ≤ len; cut pos = best_pos−seq+1 ≥ 1; pos+seq = best_pos+1 ≤ len+1 ✓. Post: ASSERT(pnl_contains_span) — n = best_pos ≤ len, no stale-read (V-009 not triggered). P4: i −= span, span ≤ i ⇒ strictly decreases, no wrap ✓. P5: miss returns 0, list untouched; hit cuts exactly seq entries ✓.

### pnl_crop_tail_sequence (pnl.c:363-375) — PROVED·A
ASSERT(len > 0) — caller defrag.c:1029 guards ✓. Span scan: guard `1+span ≤ len` precedes read ⇒ reads ≤ len ✓; cut(1, span) with 1+span ≤ len+1 ✓. P5: returns cropped span ≥ 1.

### pnl_cut (pnl.c:377-385) — PROVED (specimen, re-verified; see LEDGER SPEC-1)

### pnl_sift (pnl.c:387-431) — PROVED·A (dead export, V-011)
No callers tree-wide. Verified against its contract: scan loop reads fo[f] only after `f != fe`; advance logic cmp = pnl[r] > fo[f] ⇒ r++ else f++ — classic descending merge-filter ✓. Update loop: `remove`/`next` labels; w ≤ r always ⇒ writes pnl[w++] with w ≤ len; tail copy in-bounds; setsize(w−1) exactly once before each return from the update region; early returns leave pnl untouched ✓.
ASSUMPTIONS: A2 (strictly sorted), non-null pointers, fo checked with limit > MAX_PAGENO−1.

### pnl_cut_range (pnl.c:433-449) — PROVED·A, FINDING V-007
P1: caller defrag.c:746 passes [best_begin, best_begin+npages) with npages ≥ 1, strictly inside the domain ✓. from = pnl_search(pnl, range_end−1, MAX_PAGENO) ⇒ from ∈ [1, len+1]. Scan appends exactly the items in [range_begin, range_end−1] descending; pdest receives a sorted chunk (caller pnl_clear's temp right before, defrag.c:744 ✓). pnl_cut bounds ✓; from == len+1 ⇒ loop body never runs, no cut ✓. P4 ✓. V-007: the defrag.c:746 call site ignores the return value — provably infallible there (temp = pnl_alloc(largepage_max), alloclen ≥ 2·largepage_max ≥ npages, temp cleared) but unchecked-error hygiene (low).
ASSUMPTIONS: A2; temp reserved ≥ npages and cleared (verified defrag.c:966/744).

## src/dml.h

### da_t (dml.h:8-15) — PROVED
Bitfield domains: mapped:31 and npages:31 hold < 2^31 (page numbers ≤ MAX_PAGENO ✓); sizeof = 16, align 4 ✓. Compound-literal zero-init ✓ (dml.c:73).

### dml_search_exact (dml.h:28-31) — PROVED
Guard precedes deref; miss ⇒ nullptr. Requires sorted (descending) dml — callers pass dfc->arcs sorted at cycle start (defrag.c:953).

### dml_free (dml.h:33-38) — PROVED. Null-safe, idempotent.

## src/dml.c

### dml_size2bytes (dml.c:6-19) — PROVED·A
P1: ASSERT(size ≥ CURSOR_STACK_SIZE && ≤ PL) — dml_alloc clamps before calling; dml_extend passes 2·limit > 32 ✓. P2: doubling compensates the halving (same structure as PNL-slack); STATIC_ASSERT overflow ✓. ASSUMPTIONS: callers clamp (verified).

### dml_bytes2size (dml.c:21-28) — PROVED, FINDING V-005
Same defective upper bound as pnl_bytes2size (firing requires > ~4 TB DB; debug-only). Release: limit ≥ w ✓.

### dml_alloc (dml.c:30-43) — PROVED·A
Clamp to [CURSOR_STACK_SIZE, PL]; bytes granulated ⇒ limit = bytes2size ≥ size ✓; length=0 ✓. Caller defrag_init passes arcs_needed ≤ payload_pages ≤ MAX_PAGENO ✓.

### dml_extend (dml.c:45-60) — PROVED
P1: null → alloc(INITIAL); limit == PL ⇒ nullptr (hard cap; caller maps to ENOMEM ✓). P2: growth 2× or 1.5× clamped at PL; new limit ≥ min(size, PL) > old ⇒ progress, no oscillation ✓. P5: realloc failure returns nullptr while the caller's *pdml still owns the original block (dml_append keeps *pdml unchanged) ⇒ no leak, state consistent.

### dml_append (dml.c:62-76) — PROVED
P1: extend when length == limit; ASSERT(length < limit) after ⇒ slot exists ✓. P3/P5: post-increment write, zero-init fields; on extend failure *pdml untouched ✓.

### dml_sort (dml.c:84-87) — PROVED
Radix (tmp = items+length, 2·length ≤ slots by the dml analog of PNL-slack) else dm_sort over [items, items+length); length 0 ⇒ no-op ✓. Result descending by key_or_pgno — consistent with dm_bsearch. `unlikely(!dm_radixsort(...))` fallback dead with PREALLOC=1 — harmless.

### dml_search (dml.c:92-102) — PROVED, FINDING V-013 (info)
dm_bsearch over descending items; result ∈ [items, items+length]. The cmov extra read at length ∈ {0,1} hits items[0] (in-allocation, result-neutral) — unlike pnl_search there is no Valgrind annotation (cosmetic; V-013). `const` cast-away on return — smell, no correctness impact.

## src/txl.h / src/txl.c

### txl_alloclen / txl_size (txl.h:30-32) — PROVED. Layout identical to pnl; values ≤ txl_max ≈ 2^26 — casts lossless.

### txl_size2bytes (txl.c:6-12) — PROVED. ASSERT callers comply; bytes/8 ≥ size+2 ⇒ bytes2size ≥ size ✓; no doubling — correct, txl never radix-sorts; txl_max fits size_t ✓.

### txl_bytes2size (txl.c:14-18) — PROVED. Input ≈ request + ≤32 slack ≤ 2·txl_max ✓.

### txl_alloc (txl.c:20-33) — PROVED. Capacity 28 == txl_initial exactly ✓; len=0; nullptr on OOM.

### txl_free (txl.c:35-38) — PROVED. Null-safe.

### txl_reserve (txl.c:40-65) — PROVED·A
No null check — all in-tree paths deref txl_size first ⇒ non-null. Early-out ✓; wanna > txl_max ⇒ TXN_FULL (MPROBE_WATCH + ERROR); growth = min(2·wanna−allocated, txl_max), no underflow ⇒ capacity ≥ wanna ✓. P5: failure leaves *ptxl valid ✓.

### txl_need (txl.c:67-72) — PROVED·A. ASSERT caller passes txl_granulate=32 ✓; wanna ≤ txl_max+32; reserve handles the rest ✓.

### txl_append_prereserved (txl.c:74-78) — PROVED
Precondition size < alloclen (established by txl_append's gate) ⇒ txl[size+1] ≤ alloclen ✓; txl[0] += 1 in txnid_t — no overflow ✓.

### txl_sort (txl.c:82) — PROVED. Range exactly entries 1..len; len 0 ⇒ empty range ✓. Post-check CHECKS2-gated ✓.

### txl_append (txl.c:84-92) — PROVED
Grow only when size == alloclen (exact-fit; need(+32) guarantees headroom ⇒ prereserved safe); rc propagated; failure → no mutation ✓. If size > alloclen (corruption) the equality test skips growth and prereserved writes past capacity — guarded only by debug ASSERTs; cannot arise under the capacity invariant (self-consistent).

### txl_contain (txl.c:94-100) — PROVED. Linear scan in-bounds ✓. NOTE: no production callers — txl_* has only test callers in this tree (V-011); `__hot` moot.

## sort.h infrastructure lemmas (used by all three files)

- **SORT-search (SEARCH_IMPL, sort.h:436-482):** returns the exact lower bound — first `it ∈ [begin, end]` with `!CMP(*it, item)`; all elements before `< item`, all from `it` on `≥ item`. For length==0 returns begin with no logical-range deref; the CMOV do-while reads `*begin` even for length==0 (in-allocation, value unused); invariant `it + length ≤ end` through both variants.
- **SORT-quicksort (SORT_IMPL, sort.h:276-344):** median-of-3 network orders the triple so `*hi` ≤ pivot ≤ `*lo` ⇒ both Hoare scans stop at the pivot at the latest ⇒ no OOB; pivot-value tracking preserves `*mid == pivot`; every partition iteration shrinks [left,right] by 2, both pushed parts strict sub-ranges ⇒ termination; larger-half push ⇒ stack depth ≤ ⌊log2 n⌋+1 ≤ 33 < 64 slots; len < 8 handled by sorting networks (case split 0..8 complete; default unreachable). EXPECT_LOW_CARDINALITY=false ⇒ is_sorted path compile-time dead. Final scan CHECKS2-gated.
- **SORT-radix (RADIXSORT_IMPL, sort.h:349-412):** ≤2 passes for 32-bit keys; stable LSD counting sort; exclusive prefix sums ⇒ distribution indices < length; tmp fully written before first read; early-exit `key_diff_mask < 256` correct; END_GAP=0 with PREALLOC=1 relies on allocation slack — proven for pnl (PNL-slack) and dml (item slots ≥ 2·limit).

## Findings summary

| V-id | file:line | severity | one-line |
|---|---|---|---|
| V-005 | pnl.h:65 pnl_bytes2size; dml.c:26 dml_bytes2size | low | ASSERT upper bound ignores the pre-halving slot count (≈2×request): spurious assert for allocations > ~PL/2+slack in checking builds; release arithmetic correct |
| V-006 | pnl.c:336 pnl_get_best_sequence | low | `defrag_detent - seq` unsigned wrap if detent < seq — unguarded in-function; proven impossible at all 3 in-tree call sites; future callers would silently mis-scope |
| V-007 | defrag.c:746 (caller of pnl_cut_range) | low | return value discarded; provably infallible at that site but unchecked-error hygiene |
| V-008 | pnl.c reserve/need/alloc boundary | low | Granulation slack lets alloclen exceed PAGELIST_LIMIT by ≤ ~510 after a ≈PL-size request; len can then creep past PL; pnl_check_lite/full would reject a self-built list (debug); also trips pnl_clone's size2bytes ASSERT at the same extreme |
| V-009 | pnl.h:195-203 pnl_contains_span | info | When pgno is below every entry the bsearch returns end and `pnl[len+1]` reads a stale in-allocation byte — false positive possible; ASSERT-only usage in-tree ⇒ benign |
| V-010 | pnl.c:198/218; gc-get.c:1189 | info | Duplicate detection compiled out under MDBX_DISABLE_VALIDATION=1; strict-order precondition of merge enforced only with runtime MDBX_VALIDATION — both switches must stay aware of each other |
| V-011 | pnl.c:387 pnl_sift; pnl.c:262 pnl_intersect; txl.c | info | Dead exports: pnl_sift zero callers tree-wide; pnl_intersect tests-only; txl_* tests-only (production uses rkl) |
| V-012 | pnl.h:167-169 pnl_search | info | Null-tolerant ASSERT immediately precedes an unconditional deref — misleading guard; safe in-tree (repnl allocated at txn-basal.c:95) |
| V-013 | dml.c:92-102 dml_search | info | Benign cmov overread of items[0] at length ∈ {0,1} without the Valgrind annotation pnl_search has |

No high/critical findings, no REFUTED in this tranche. Verdict census: 43 rows — 36 PROVED, 7 PROVED·A (rounded up: setsize/search/contains_span/append-family/insert_span/get_best_sequence/crop_tail/sift/cut_range/merge-family/dml-txl variants), 0 REFUTED, 0 SUSPECT.

## Established lemmas (reusable upward)

- **PNL-slack:** any pnl buffer from alloc/reserve/shrink with request w satisfies raw slots S ≥ 2·alloclen+3 and alloclen ≥ w ≥ 1 (bytes = ceil_p2(16+4(2w+3),4096)−16 ≥ 8w+12). Consequently entries 1..alloclen writable, radixsort's in-buffer tmp (entries len+1..2·len) in-bounds, cmov bsearch one-past read (len+1 ≤ S−2) in-allocation. (dml analog: item slots ≥ 2·limit.)
- **PNL-capacity:** for every live list, len = pl[0] ≤ alloclen = pl[-1]; maintained by reserve's `*pnl ≥ wanna` postcondition, need-gated appends, monotone cut/shrink/sift, clone; granularity-slack exception → V-008.
- **PNL-order:** wr.repnl is strictly-descending with entries in [MIN_PAGENO, limit) at every consumption point; producers either order-preserving (insert_span under A1, merge under A2) or followed by explicit pnl_sort (defrag.c:182/660/761, gc-put.c:382/461 for retired).
- **detent ≥ seq:** at every pnl_get_best_sequence call, defrag_enough > payload_pages ≥ (sum of overflow pages) ≥ any single arc npages = seq; defrag_repnl_get additionally enforces defrag_enough ≥ LEAST(repnl)+npages.
- **A1 (span-disjointness):** span/stepped/insert appends never overlap existing entries — appended pages are dirty/loose/retired/spilled and hence disjoint from reclaimed/free lists by the page-state machine; global order re-established at consumption boundaries.
- **A2 (strictly sorted merge inputs):** required by pnl_merge_inner's a==b duplicate detection; guaranteed for GC-loaded lists only under runtime MDBX_VALIDATION (else lite) — see V-010.
- **A3–A5:** txl_reserve never sees null; dml_alloc size ≤ PL; search/sift/merge lists are live pointers. Verified at all in-tree call sites.
- **A6 (verified, promoted):** gc-put.c loose-merge overlap safety — pnl_need(&repnl, 2·loose+2) ⇒ merge write range [1..len+loose] strictly below the embedded src window [alloclen−loose, alloclen−1].
