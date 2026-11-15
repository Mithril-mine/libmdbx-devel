# Verification tranche V7 — statement-level P1–P7 ledger, gc-get.c + gc-put.c (stream A top-up)

Work tree: /home/doom/Projects/libmdbx-audit (branch verification/master-audit). Files: `src/gc-get.c` (1561 lines), `src/gc-put.c` (1560 lines).
This completes the per-function P1–P7 ledger for the functions the adversarial pass (G7, `G7-gc-adversarial.md`,
V-002..V-004/V-022) did **not** ledger per-function, and formalizes items it only held empirically or property-wise.
Cross-checks of already-adversarially-held functions are marked *reuse* and are not re-proven here.

Build facts (confirmed in this pass): `MDBX_PNL_ASCENDING = 0` (descending PNL is the only live variant; ascending
blocks are compile-time dead), `MDBX_ENABLE_BIGFOOT = 1` default, `MDBX_USE_MINCORE = 1` default (Linux),
`MDBX_ENABLE_SAVING_SEQUENCES = 0` (gc-get.c:629, dead branch), `ALLOC_EXACTLY` undefined (gc.h:61 `#if 0`),
`MDBX_MIN_PAGESIZE = 256` (mdbx.h:868), `indx_t = uint16_t` (layout-dxb.h:57), `node_t = 8 bytes` (layout-dxb.h:227-246),
`MIN_TXNID = 1`, `INVALID_TXNID = UINT64_MAX` (layout-dxb.h:51-54), `MDBX_IS_ERROR(rc)` false for RESULT_TRUE/FALSE
(essentials.h:256), `lck->mincore_cache = {pgno_t begin[4]; uint64_t mask[4];}` (layout-lck.h:264-268).
Derived constants: `maxgc_large1page = (ps-16)/4 - 1` (env.c:33-35, ≥ 43 by ENSURE), `maxgc_per_branch =
(ps-16)/(2+8+8) ≥ 13` (env.c:37) — the latter is load-bearing for the stockpile4retired termination proof below.
`goodchunk = maxgc_large1page - maxgc_large1page/16` (gc-put.c:9).

P1 conditions, P2 arithmetic, P3 memory, P4 termination, P5 exits, P6 concurrency, P7 format/API-contract.

---

## 1. gc-get.c ledger

| # | function (lines) | P1 | P2 | P3 | P4 | P5 | P6 | P7 | verdict |
|---|---|---|---|---|---|---|---|---|---|
| G1 | `bit_tas` (11-16) | total fn of (*field*, bit 0..63) | shift ≤ 63 by caller contract | field valid by construction | O(1) | returns old bit | see LEM-MINCORE-TAS | TAS = read+OR | **PROVED** |
| G2 | `mincore_fetch` (18-80) | complete; miss ⇒ slot rotation or syscall | see §1.1 | vector sized exactly `pages` bytes; length clamped to mmap current | all loops bounded (i<4; do-while decrement; i<pages) | false on mincore() error, cache untouched on that path (store happens after the syscall, :60-79) | lck-shared non-atomic race — hint-only consumer (G7 reuse) | mask bit d = "db-page d fully in core" (inverted vote over os-pages) | **PROVED** |
| G3 | `env_is_page_incore` (83-98) | hint contract: answer gates prefault/pread choice only | `floor_powerof2(bytes, sys_ps)` then `>> max(ps2ln, sys_ln2)`; eASSERT `unit<<log == offset_aligned` holds (floor/shift inverse) | no raw vector access | O(1) + fetch | bool | benign race (G7) | same bit semantics as G2 | **PROVED-A** (hint contract, §1.2) |
| G4 | `scan4seq_fallback` (102-164) | complete case split, §1.3 | no-wrap equality (§1.3) | reads confined to `[pnl+1, pnl+len]` and partners `[1, len-seq]` | every candidate position checked exactly once; detent residue argument | nullptr ⇔ no run | n/a (single-threaded writer) | returns highest-index run start, cut-compatible (wave1 G7) | **PROVED** |
| G5 | `scan4range_checker` (166-182) | same candidate set `[seq+1, len]` | same no-wrap equality | reads `pnl[begin-seq], pnl[begin]` with begin > seq | begin strictly decreases to seq | nullptr ⇔ no run | n/a | same first-match-from-top direction as scanners ⇒ agreement on sorted input (debug assert G4/G5) | **PROVED** |
| G6 | SIMD variants sse2/avx2/avx512/neon (184-499) | — | — | — | — | — | — | — | *reuse G7* (x86 ATTACKED→held; NEON **V-002 REFUTED**) |
| G7 | `is_reclaimable` (557-572) | 3-case split, §1.4 | none (no arithmetic) | none | O(1) | false paths set `txn_gc_drained` (only the empty-tree case) | flags stable in writer CS | `STATIC_ASSERT(ALLOC_RESERVE == z_gcu_preparation)` ties the two flag domains (gc-put.c:560 vs cursor.h:121) | **PROVED** |
| G8 | `repnl_get_single` / `gc_repnl_get_single` (574-673) | — | — | — | — | — | — | — | *reuse G7* (SAVING_SEQUENCES dead; truncation path) |
| G9 | `gc_repnl_scan_sequence_reserve` (675-715) | reserve = pure probe (no cut): `pgno = *target` returned, list untouched (:711) | seq = num−1 ≥ 1 (num > 1 asserted) | probe reads only | scan4seq bounded (G4) | 0 ⇔ no run; `gc_repnl_has_span` consumes only the != 0 answer | n/a | RESERVE callers (alloc_ex :996/:1150, has_span :775) discard pgno → no consumption anywhere | **PROVED** |
| G10 | `gc_repnl_get_sequence` (717-772) | RESERVE dispatches to G9 | — | — | — | — | — | cut loop = wave1 G7 LEM (cut/paste) | *reuse* (cut HELD; ALLOC_EXACTLY dead) |
| G11 | `gc_repnl_has_span` (774-776) | num ≤ 1 ⇒ non-emptiness, num > 1 ⇒ run probe | — | — | G9 bounded | bool | n/a | — | **PROVED** |
| G12 | `page_alloc_finalize` (778-912) | — | — | — | — | — | — | — | *reuse G7* (prefault batching HELD + quirk) |
| G13 | `snapshot_oldest_force_rescan` (914-917) | side effect: `rdt_refresh_flag = true` (mo_AcquireRelease) forces every scanner's signature check to fail → rescan; `may_lag=false` ⇒ full stable-pass scan (mvcc-readers.c:108-132), no lagged shortcut | none | atomic flag store only | bounded by reader-table passes | oldest_txnid (used ONLY in the grow-path NOTICE, gc-get.c:1440-1444) | readers scan concurrently with safe64 loads — safe | — | **PROVED** |
| G14 | `gc_check_keylen` (919-921) | exactly `key_len == 8` | — | — | O(1) | reason string or nullptr | n/a | paired with `gc_peekid` (P1 identical check, gc-put.c:24) | **PROVED** |
| G15 | `gc_check_rowdata` (923-939) | 5-clause case split, §1.5 | len%4, len≥4 before `MDBX_PNL_SIZEOF(pnl)` reads pnl[0] — **order is load-bearing** | payload read only after length+alignment gates | O(1)/O(n) by `validate` | reason string or nullptr | n/a | strength = VALIDATION-dependent (V-022 documented compromise) | **PROVED** |
| G16 | `gc_row_pnl` (941-948) | reason ⇒ err=CORRUPTED, pnl=null | — | — | O(1) | glr_t total | n/a | — | **PROVED** |
| G17 | `gc_cursor_init` (950-957) | `dbi_state = txn->dbi_state` (array base) ⇒ `cursor_dbi(mc) = ptr_diff = 0 = FREE_DBI` (cursor.h:342); `tree = txn->dbs = &dbs[0] = FREE_DBI tree` | none | pointer into basal_txn tail — allocation covers it (txn-basal.c:75 `sizeof(MDBX_txn) + sizeof(cursor_couple_t)`, LEDGER L5) | O(1) | fresh cursor (`z_fresh_mark` = poor+fresh), NOT registered in txn->cursors; registration happens only around the early-clean `cursor_del` (gc-alloc :1249-1256, txn_nipped guard) | untracked cursor used for reads only — legal | — | **PROVED** |
| G18 | `gc_alloc_ex` (959-1527) | — | — | — | — | §1.6 matrix | steady-wipe/detent retries monotone (G7 (d) reuse) | USDT/DTRACE args well-defined (pgno ? : tail) | **PROVED** (matrix); assumptions A1-A3 below |
| G19 | `gc_alloc_single` (1529-1561) | loose loop: each iteration pops one loose page (list shrinks) or returns | loose pgno asserts `≥ NUM_METAS, < first_unallocated` | page stamped `front_txnid` | loop terminates; break handles refund-consumed-loose | 3 exits, all pgr_t total (§1.6 tail) | writer CS | asserts F_ISSET all-bits (LINDO|VALID|DIRTY) | **PROVED** |

### 1.1 mincore_fetch arithmetic (G2)

- Slots: `begin[4]/mask[4]`; hit test scans i = 1..3 (`dist = unit_begin - begin[i]`, rejected when negative or ≥ 64;
  `dist ∈ [0,64)` fits `char`).
- Move-to-front do-while (:25-28): copies slot i ← slot i−1 while decrementing i to 0, then restores `tmp` into slot 0
  — exact MRU rotation; `i` is the for-body variable, loop exits at i == 0 before the outer loop can resume.
- Unit geometry: `unit_log = max(env->ps2ln, sys_ln2)`; `shift = ps2ln − sys_ln2` (0 when ps ≤ sys); `pages = 64 << shift`;
  `offset = unit_begin << unit_log` (== offset_aligned, exact by G3's assert), `length = pages << sys_ln2` (== 64 db-pages);
  clamp `offset + length > current ⇒ length = current − offset, pages = length >> sys_ln2` (offset < current strictly,
  since it is derived from an allocated pgno; length ≥ ps ≥ sys_pagesize > 0 after clamp).
- Vector: `osal_alloca(pages)` = exactly the number of os-pages mincore() writes (`length / sys_pagesize`), build loop
  reads `vector[0..pages)`.
- Mask build: `bit = (vector[i] & 1) == 0` (1 = some os-page of the group NOT in core), `bit <<= i >> shift` (≤ 63),
  `|=` accumulates the inverted vote per db-page group; stored `mask[0] = ~mask` ⇒ bit d set ⇔ all os-pages of db-page
  d resident. On syscall failure nothing is cached.

### 1.2 The TAS side effect and why it is sound (LEM-MINCORE-TAS)

`bit_tas` sets the bit even when the answer is false. So every queried unit becomes cached "in-core" until evicted.
This is coherent **by consumer analysis**: the only consumers are (a) the prefault-write path (gc-get.c:823/841) —
after a false answer it writes the stub, which makes the page in-core, so the cached bit records the post-action
state; (b) defrag source-page selection (defrag.c:483/522) — after a false answer it `pread`s, which fills the page
cache backing the mmap (content-equivalent; the cached true only switches pread→memcpy, both correct). No consumer
uses the answer for anything but a performance choice. Cross-process tearing of `begin[]/mask[]` is benign for the
same reason (G7 reuse). Verdict **PROVED-A**: sound under the hint contract; not a correctness oracle.

### 1.3 scan4seq_fallback completeness (G4, descending live variant)

- Candidate positions: element index j admits a check iff its partner j−seq ≥ 1 (never read pnl[0]), i.e. j ∈
  `[seq+1, len]` = `[detent+1, edge]` with `detent = range − len + seq = &pnl[seq]`.
- Equality: `diff = a − b (mod 2³²) == (pgno_t)(−seq)` ⇔ `a == b − seq` exactly — no false positive for real pgnos
  (both sides in (−2³², 2³²); wrap only admits the exact match). No signedness hazards: `pgno_t` unsigned, `offset`
  ptrdiff_t negative index only.
- Main loop: body at `range` checks indices len..len−3 (first), then `range -= 4`, continue iff `range > detent+3`,
  so every body has all four positions ≥ detent+1 (valid). Exit value `range ∈ [detent, detent+3]` — provable since
  `range − detent ≡ (len − seq) mod 4` and the exit test fires at the first value ≤ detent+3 (never below detent).
  `range == detent ⇒` all candidates already checked ⇒ nullptr.
- Tail do-while checks `range, range−1, …, detent+1` (stops at detent — correctly excluded as invalid).
- Coverage: each candidate position checked exactly once, in top-down order; returns the highest-index match, which is
  exactly what `scan4range_checker` returns ⇒ the debug assert `target == scan4range_checker(...)` (gc-get.c:688/736)
  holds on sorted descending input. Cut compatibility = wave1 G7 LEM.

### 1.4 is_reclaimable case split (G7)

1. `mc->tree == &txn->dbs[FREE_DBI] && !((flags | mc->flags) & z_gcu_preparation)` ⇒ **false**: a GC-cursor
   allocation that is neither the reserve flags nor under `gc_touch`'s preparation flag — the "retired-list cannot
   play catch-up with itself" recursion guard. Inclusive-OR bit test covers both flag carriers.
2. Otherwise, `FREE_DBI.items == 0` ⇒ set `txn_gc_drained`, **false** (empty GC; issue #31 guard).
3. Else **true** (non-GC cursor, or GC cursor in preparation mode).

### 1.5 gc_check_rowdata case split (G15)

1. `iov_len % 4 != 0 || iov_len < 4` → "invalid length" (the `< 4` check must precede `MDBX_PNL_SIZEOF(pnl)`, which
   dereferences `pnl[0]`).
2. `(uintptr_t)iov_base % 4 != 0` → "invalid alignment" (defensive vs crafted node geometry).
3. `iov_len < MDBX_PNL_SIZEOF(pnl)` → "trimmed" (record does not hold `pnl[0..len]`).
4. `validate ? pnl_check_full : pnl_check_lite` (limit = first_unallocated) — lite skips all reads for len 0 and
   checks bounds+MOST/LEAST only; full adds the strict order scan. Strength documented in-source (:1196-1222) and by
   V-022/PAIR-F3 (crafted interior disorder passes lite).
5. else accept.

### 1.6 gc_alloc_ex exit-consistency matrix (G18) — formalizing wave-1 properties (a)–(d)

Every exit, its error level / TXN_ERROR effect, and state consistency:

| exit | reached from | ret.err | TXN_ERROR set | page state / consumption | notes |
|---|---|---|---|---|---|
| `done` (1456) via early probe :997-998 or SHOULD_SCAN :1147-1152 or scan :1300-1306, `pgno != 0` | repnl consumption (cut/single) | SUCCESS | no (only set later at :1491 if finalize fails) | repnl reduced by exactly num; `pgno+num ≤ first_unallocated` asserted; page_alloc_finalize | the only exits that return a page — no double-return (no backward edges from done; reserve probes land in reserve_done) |
| `done` tail (:1395-1397) and grow-path fall-through :1452→done, `pgno == 0` | no_gc tail / grow | SUCCESS | no | `pgno = first_unallocated; first_unallocated += num` (≤ end_pgno asserted; grow updated env->txn->geo.end_pgno at :1450) | A1 below |
| `reserve_done` (1518) from scan :1287-1292 (RESERVE && num < 2) | slot/id requests | SUCCESS | no | page = nullptr; repnl only **grown** (merges) — intended for stockpile | G7 (c) formalized: every `num == 0` path reaches this or `fail`, never allocates |
| `reserve_done` from slot-skip :1131-1132 | !num and record too long | MDBX_NOTFOUND | no | page = nullptr, nothing consumed | callers treat NOTFOUND as "no slot" |
| `fail` (1476) — from: detent>snapshot :1042; cursor_ops hard error :1081; keylen corrupt :1089; node_read :1111; gc_row_pnl :1116; pnl_need :1176; has_duplicates :1226; CHECKS2 pnl :1234; rkl_push :1275; steady wipe/make-steady :1360/:1378; resize :1448; no_gc RESERVE :1416 / BACKLOG_DEPLETED :1411 / MAP_FULL :1428; finalize error :1475 | any | per-site | iff neither UNIMPORTANT nor RESERVE (:1486-1493) | page = nullptr; no partial page is ever returned; partial merges stay in in-txn state (repnl/rkl/GC-record delete commit only with the txn — roll back on abort); refund-if-adjacent epilogue :1479-1483 when !MDBX_TXN_ERROR | A2 below (UNIMPORTANT quirk); `num > 1` adds to `gc.spent` :1515-1516 |
| `no_gc`→fail with RESERVE (NOTFOUND) | depleted+reserve | NOTFOUND | no | — | `gc_prepare_stockpile` maps NOTFOUND→SUCCESS (:259): tolerated degradation |

**A1** (PROVED-A): line 1450 writes `env->txn->geo.end_pgno`. Sound because `mc->txn == env->txn` for every
gc_alloc_ex call: write txns run with `env->txn = txn` (txn-basal.c:207, txn-nested.c:350) under the writer mutex,
and GC allocation is writer-only. The indirection is otherwise equivalent to `txn->geo.end_pgno`.
**A2** (documented quirk, G7(b)): MDBX_CORRUPTED during an ALLOC_UNIMPORTANT reserve logs at DEBUG and does not set
MDBX_TXN_ERROR — but the error still propagates (gc_rerere/gc_update return it) and the commit fails; benign.
**A3** (reuse G7(d)): the `retry_gc_refresh_detent / retry_gc_have_detent / next_gc ⇄ scan ⇄ depleted_gc` loop is
monotone (detent forward-only, troika advances per steady fire, SHOULD_SCAN cleared after the second depleted pass).

gc_alloc_single exits (G19): (i) loose page returned (SUCCESS; loose list/count decremented; page stamped
`front_txnid`; `first_unallocated` untouched — loose pages are pre-accounted); (ii) repnl single via
`page_alloc_finalize` (err per finalize); (iii) `gc_alloc_ex(mc, 1, ALLOC_DEFAULT)` — reached only when repnl is
empty, satisfying alloc_ex's :1001 precondition. The refund loop (`loose_refund_wl > first_unallocated →
txn_refund`, break when it consumed all loose) preserves the loop measure (list shrinks or returns).

---

## 2. gc-put.c ledger

| # | function (lines) | P1 | P2 | P3 | P4 | P5 | P6 | P7 | verdict |
|---|---|---|---|---|---|---|---|---|---|
| P1f | `gc_put_init` (6-15) | `memset(ctx, 0, offsetof(gcu_t, sequel))` zeroes every scalar field before `sequel`; `sequel` itself initialized by `rkl_init` immediately after — no gap; `dense_histogram` left uninit but `gc_dense_hist` memsets before use | goodchunk = maxgc*15/16 (≥ 40 given maxgc ≥ 43) | — | O(1) | returns cursor_init's err | — | bigfoot seed = txn->txnid | **PROVED** (G7 note formalized) |
| P2f | `gc_put_destroy` (17) | — | — | frees sequel arena | O(1) | caller txn-basal.c:400-418 calls it on ALL paths (success/failure) after gc_update | — | — | **PROVED** |
| P3f | `gc_chunk_pages` (19-21) | total function | `largechunk_npages(env, (chunk+1)*4)` = ceil((15+(chunk+1)*4)/ps)+1 — conservative upper-bound page count (safe direction for reserve math); no 32-bit wrap for all legal chunk ≤ PAGELIST_LIMIT-scale inputs on 64-bit | — | O(1) | pages | n/a | estimator, not exact (exact-fit edge gives one extra page) | **PROVED** |
| P4f | `gc_peekid` (23-30) | `iov_len == 8` gate before the u64 peek; else CORRUPTED | — | `unaligned_peek_u64(4, base)` reads exactly the checked 8 bytes | O(1) | CORRUPTED+log | n/a | mirror of G14 | **PROVED** |
| P5f | `dbg_prefix` (44-46) | pure fn of `env->flags & MDBX_LIFORECLAIM` (stable in writer CS) | — | — | O(1) | string | — | — | **PROVED** |
| P6f | `dbg_id` / `dbg_dump_ids` (48-129) | logging blanket under MDBX_DEBUG_GCU | — | — | cursor walk bounded by GC items | logs only | — | **N-A** (debug-only) + **F-V7-1** nit: `id` may be read uninitialized at :94 if `cursor_init`/`outer_first` fails inside the `items > 0` branch (diagnostic-only garbage) |
| P7f | `gc_clean_stored_retired` (159-186) | deletes stored bigfoot-slice records for ids `txn->txnid .. ctx->bigfoot`, NOTFOUND tolerated per id | unsigned `--ctx->bigfoot >= txn->txnid`: bigfoot ≥ 1 (MIN_TXNID) ⇒ at `bigfoot == txnid` the decrement wraps to UINT64_MAX < txnid and exits — no pass-through | seek/del on valid cursor | bigfoot strictly decreases ⇒ ≤ range iterations; each id deletes ≤ 1 record | retired_stored zeroed **before** del (idempotent vs failed del — re-store overwrites the stale record; error → txn error anyway) | writer CS | key = bigfoot (BIGFOOT) / txnid; MDBX_SET exact | **PROVED** |
| P8f | `gc_touch` (188-198) | pre: is_pointed or leaf_pages==0 (callers ensure); `cursor_touch` = make-dbi-dirty + spill-estimate + page_touch the whole path; key/data used for **length-only** spill estimate (`node_size` reads iov_len; null bases legal) | `flags += / -= z_gcu_preparation` is exact OR/AND-NOT: no outer-cursor state has bit 0x02 (zone enum, cursor.h:100-165), and gc_touch is not nested ⇒ INV-PREPFLAG below | — | O(tree height) | err from cursor_touch | preparation flag transiently visible to is_reclaimable (intended) | — | **PROVED** |
| P9f | `gc_reclaim_slot` (200-203) | `gc_alloc_ex(cursor, 0, RESERVE|UNIMPORTANT).err` — slot request, asserts num>0⇒RESERVE hold (gc.h:55-63, alloc_ex :976) | — | — | alloc_ex bounded (A3) | NOTFOUND tolerated by callers (dense :1118 falls to reserve4retired) | — | — | **PROVED** (thin) |
| P10f | `gc_reserve4retired` (205-208) | `gc_alloc_ex(len, RESERVE|UNIMPORTANT).err` | — | — | ditto | ditto | — | — | **PROVED** (thin) |
| P11f | `gc_reserve4stockpile` (210-213) | `gc_alloc_ex(1, RESERVE|UNIMPORTANT).err` | — | — | ditto; progress lemma LEM-STOCKPILE below | ditto | — | — | **PROVED** (thin) |
| P12f | `gc_prepare_stockpile` (215-261) | budget arithmetic §2.1; `continue` path at most once (clean zeroes retired_stored; second pass skips the clean branch) | all-add budget; no subtraction anywhere — no unsigned wrap | — | §2.1 | NOTFOUND→SUCCESS (fail-safe); other err propagated | — | — | **PROVED** |
| P13f | `gc_prepare_stockpile4retired` (265-287) | for_retired = ceil(retired_left/goodchunk) leaf chunks + b-tree escalation; outer_last NOTFOUND tolerated (empty GC) | escalation loop terminates since `maxgc_per_branch ≥ 13 ≥ 2` (§2.2); geometric sum ⇒ for_retired < 1.1×chunks0 — no overflow | — | §2.2 | delegates to P12f | — | budget is an estimate (undercounts the final root page — tolerable: stockpile is re-checked after touch and gc_update retries) | **PROVED** |
| P14f | `gc_merge_loose` (289-361) | — | — | — | — | — | — | — | *reuse pairs-wave1* (CONSISTENT·A; PAIR-N2 disjointness, PAIR-F1/V-018 ignored-return note) |
| P15f | `gc_store_retired` — **non-BIGFOOT branch** (436-465) | retry loop: reserve record sized to current PNL; retry iff PNL grew during the RESERVE put | `data.iov_len` strictly increases per retry (loop guard `iov_len < PNL_SIZEOF`), bounded by PAGELIST_LIMIT ⇒ terminates | memcpy copies exactly `data.iov_len == MDBX_PNL_SIZEOF` bytes **after** the exit and after `pnl_sort` (sort does not change size); source in-bounds | measure: record size strictly ↑, bounded | errors returned (bailout at gc_update :1494); retired_stored set only on success | — | record = full PNL image incl. size field; sorted at write time; consumers (gc_check_rowdata) read the same format | **PROVED** |
| P16f | `gc_store_retired` — BIGFOOT arithmetic summary (367-435) | chunk_hi = goodchunk while txnid headroom (`bigfoot < MAX_TXNID−UINT32_MAX` ⇒ slice ids can never reach INVALID_TXNID; slices ≤ PAGELIST_LIMIT/3 ≪ 2³²), else `left_before|3` (≤ goodchunk ⇒ record fits ONE page ⇒ RESERVE never splits) | slice loop: `chunk = min(left_after, chunk_hi) ≥ 1` per iteration; `retired_stored` strictly ↑ ⇒ inner loop terminates; FIFO `at = left_before − chunk` / LIFO `at = retired_stored` | — | inner loop PROVED; outer `should_retry` PROVED-A (growth bounded per txn; re-store strictly re-advances; ultimate bound = gc_update's 42-loop) | errors returned | — | **`at` formula: reuse V-003 (REFUTED, high, FIFO)** — `left_before` is stale when the list grew inside the RESERVE put's window and the growth still fits the chunk (`retired_after ≤ left_before|3`, first chunk); LIFO immune; fix shape `retired_after − chunk`. Everything else on this path holds | **PROVED / V-003 reuse** |
| P17f | `gc_remove_rkl` (477-521) | edge id (lowest FIFO / highest LIFO); `ctx->gc_first == id ⇒ gc_first = 0` (cache invalidation consistent with gc_search_holes :822-833); NOTFOUND ⇒ pop+recycle to ready4reuse (record already gone) | — | — | the `id != rkl_edge` re-check (:506-509) is deterministically dead (no rkl mutation runs between edge read and re-check inside this call: seek/prepare touch only the tree) ⇒ at most one extra pass, no livelock; NOTFOUND branch pops (shrinks rkl) | all errors propagated; ENSURE_OBJ on pop | writer CS | MDBX_SET exact seek; delete → push to ready4reuse | **PROVED** |
| P18f | `gc_clear_reclaimed` / `gc_clear_returned` (523-531) | thin; `gc_clear_returned` zeroes `return_reserved_lo/hi` **before** removing comeback — the re-loop accounting restart invariant used by every retry (gc_update :1455; G7 convergence note) | — | — | gc_remove_rkl bounded (P17f) | propagate | — | comeback ids recycled to ready4reuse | **PROVED** |
| P19f | `gc_push_sequel` (533-547) | asserts 0 < id ≤ detent; debug-dup guard vs comeback/ready4reuse; rkl_push duplicate ⇒ PROBLEM (fail-safe) | `return_left -= goodchunk` per push; RESULT_TRUE exactly when `return_left ≤ 0` — the contract asserted by both callers (:907, :1211); return_left bounded below by −goodchunk | — | — | SUCCESS / RESULT_TRUE / PROBLEM / ENOMEM | — | — | **PROVED** |
| P20f | `gc_search_holes` (819-954) | — | — | push loop `while (--end >= begin)` cannot wrap: entered with `begin ≥ 1` ⇒ decrements stop at `begin−1 ≥ 0` failing `≥ begin`; `MIN_TXNID >= begin` break guards the end underflow | §2.3 measures; push count bounded by return_left (RESULT_TRUE early exit) | RESULT_TRUE contract (P19f); errors propagate | writer CS; cursor resumed across branches (PREV state preserved) | free-id enumeration = 3-way min/max of hole iterators + scan window | **PROVED** + nit **N-V7-2** |
| P21f | `gc_reserve4return` (956-1006) | NOOVERWRITE is structurally absent-key: id was popped from ready4reuse (pushed only after cursor_del of that record) | runtime guard :962-966: popped 0 (empty pop, rkl.c:407-409) or id > cached_oldest ⇒ MDBX_PROBLEM — fail-safe, matches G7 "attacked and not broken" | reserved record memset 0 immediately (no stale-data window before gc_fill_returned) | single put per call | err paths propagate; RESULT_TRUE restart when reclaimed rkl non-empty (accounting already updated — caller re-loops) | writer CS | record size = gc_chunk_bytes(chunk_hi); comeback push before put | **PROVED** |
| P22f | `dense_chunk_outlay` (1008-1011) | need_span = P3f; span presence via G11 | — | — | scan bounded | 0 when no such span (caller falls back) | — | — | **PROVED** |
| P23f | `dense_adjust_chunk` (1013-1029) | bsearch over `adjusted` for `chunk − outlay(adjusted) ≈ adjusted`; predicate monotone: outlay(·) non-decreasing ⇒ probe non-increasing vs increasing RHS | `lo = chunk − gc_chunk_pages(chunk) − 1`: `gc_chunk_pages(chunk) ≤ chunk` for chunk ≥ maxgc+1 ≥ 44 ⇒ no wrap; bsearch convergence: `adjusted = (hi+lo)/2 ≥ lo`; both `lo=adjusted+1` and `hi=adjusted−1` shrink `[lo,hi]`; exits ≤ ⌈log₂⌉+1 iterations | — | P4 PROVED | returns last `adjusted` (predicate may be off-by-a-bit — callers only use it as a chunk-size hint) | — | — | **PROVED** |
| P24f | `dense_adjust_amount` (1031-1035) | total | result ≥ amount in both arms (`maxgc` > amount+gap; `amount+snubber`); monotone non-decreasing; `+gap+snubber ≤ +8K` — no 64-bit wrap | — | O(1) | size | — | chunk-size snubber | **PROVED** |
| P25f | `gc_handle_dense` / `gc_rerere` / `gc_fill_returned` / `gc_update` / `solve_*` | — | — | — | — | — | — | — | *reuse G7* (V-004 REFUTED liveness; memory HELD; fill/convergence HELD). New P2 notes folded into N-V7-2/N-V7-3 below |
| P26f | `gc_enforce_not_spilled` (1270-1308) | loop: seek+spill-remediate until the record's page is not spilled / not in the map | — | — | each remediation un-spills the page (finite per txn) or breaks | err propagate | writer CS | — | **PROVED** (thin; G7 context) |

### 2.1 gc_prepare_stockpile budget arithmetic (P12f)

`for_cow = FREE_DBI.height; for_rebalance = for_cow + 1 + (height+1 ≥ branch_pages); for_tree_{before,after}_touch =
…; for_all_{before,after}_touch = for_retired + for_tree_*`. All quantities size_t, additions only — no subtraction ⇒
no wrap domain issues. Early success: `for_retired < 2 && gc_stockpile > for_all_before_touch` (stockpile =
repnl + loose_count, gc.h:70-72). The reserve while-loop (`stockpile < for_all_after_touch`) terminates by
LEM-STOCKPILE: each successful `gc_reserve4stockpile` (num=1, RESERVE) either grows repnl (alloc_ex merges ≥ 1 GC
record before its only num==1 RESERVE success exit, :1143-1149; the scan exit at :1300 is assert-gated to
non-RESERVE) or consumes one GC record id into the reclaimed rkl (finite supply); exhaustion ⇒ MDBX_NOTFOUND exits
the loop and is mapped to SUCCESS.

### 2.2 prepare_stockpile4retired escalation termination (P13f)

```c
for_retired += (retired_left + goodchunk - 1) / goodchunk;   // chunks0, goodchunk ≥ 40, retired_left > 0
for (size_t entries = for_retired; entries > 1; for_retired += entries)
  entries = (entries + per_branch_page - 1) / per_branch_page;
```
`per_branch_page = maxgc_per_branch = (ps−16)/(sizeof(indx_t)+sizeof(node_t)+sizeof(txnid_t)) ≥ (256−16)/(2+8+8) = 13`
for every legal pagesize (MDBX_MIN_PAGESIZE = 256, env.c:37). For `entries ≥ 2`, `ceil(entries/13) < entries` ⇒
`entries` strictly decreases ⇒ termination; the added totals form a geometric series with ratio 1/13 ⇒
`for_retired ≤ 1.09 × chunks0`, no overflow. The final root page is not added (loop exits at entries == 1 before
adding it) — a benign undercount of an *estimate*: the stockpile is re-checked after gc_touch and shortages re-enter
gc_update's retry loop.

### 2.3 gc_search_holes termination measures (P20f)

State: `end = min(scan_hi, hole_ready4reuse.end, hole_comeback.end)` (candidates pushed: `[begin, end−1]`,
`begin = max(scan_lo, hole_ready4reuse.begin, hole_comeback.begin)`).
- The three `begin` sources are ordered by the branch guards: turn-comeback fires when `hole_comeback.begin ≥ end`,
  turn-ready4reuse likewise, turn-scan when `scan_lo ≥ end`; entering the push branch ⇒ all three `begin` < `end` ⇒
  `begin ≤ end−1` ⇒ ≥ 1 id pushed per push iteration.
- Every outer iteration strictly decreases the *next* iteration's `end`: turn branches pull the strictly-lower next
  hole (`new hole.end < begin ≤ end−1`); push branches pull holes whose `begin` was touched or advance the scan
  window (`scan_hi = scan_lo−1 ≤ end−2`, or the SET_RANGE/PREV walk lowers `scan_lo` below `end−1` within one extra
  iteration, after which the push branch runs); the max-of-three `begin` always equals at least one source, so some
  source always advances.
- Inner scan do-while (:924-948): each pass cursor-PREVs to a strictly smaller id or breaks on threshold-stop /
  NOTFOUND; exit state `scan_lo = P+1 ≤ scan_hi` is a genuine free window; the cursor position is preserved across
  outer iterations (PREV resumes without re-seek).
- Global bound: pushes stop at `return_left ≤ 0` (RESULT_TRUE via gc_push_sequel), ids bounded below by MIN_TXNID=1
  (`MIN_TXNID ≥ begin` break), hole iterators bounded by rkl lengths. ⇒ termination. (The adversarial pass held this
  empirically; these are the formalized measures.)

**N-V7-2** (nit, perf-only): `reasonable_deep = maxgc_per_branch + 2*(detent − txnid_min(rkl_lowest(ready4reuse),
rkl_lowest(comeback)))` — `rkl_lowest(empty) = INVALID_TXNID = UINT64_MAX` (rkl.c:412-416) is an intentional
+∞ sentinel for the `min()`, but when **ready4reuse is empty** the subtraction wraps (`detent − UINT64_MAX = detent+1
mod 2⁶⁴`), yielding a huge `reasonable_deep` ⇒ `scan_threshold = 0` ⇒ the threshold early-stop is disabled (scan
proceeds to EOF/`tail_space`-free behavior; termination unaffected, results unaffected). Reachable: gc_search_holes
is entered precisely when slots are insufficient, so an empty ready4reuse is possible. Cosmetic; a
`txnid_min(…, detent)` clamp would restore the optimization.

---

## 3. Findings

| id | file:line | severity | description |
|---|---|---|---|
| F-V7-1 | gc-put.c:83-94 (`dbg_dump_ids`) | nit (debug-only) | inside `if (ctx->cursor.tree->items)`, if `cursor_init` or `outer_first` fails (the `err` is only logged), the loop is skipped and `id` is read uninitialized at the final `(id > detent) ? "..." : ""` print. MDBX_DEBUG_GCU builds only; diagnostic garbage, no release impact. Initialize `txnid_t id = 0;` to silence. |
| N-V7-2 | gc-put.c:855-858 (`gc_search_holes`) | nit (perf-only) | INVALID_TXNID sentinel wraps the `reasonable_deep` subtraction when ready4reuse is empty ⇒ `scan_threshold = 0` disables the threshold early-stop. Termination and results unaffected (EOF stop + tail_space remain). |
| N-V7-3 | gc-get.c:11-98 (mincore block) | info (design note) | The test-and-set semantics of `bit_tas` cache the **post-action** state (prefault write / pread). Documented as LEM-MINCORE-TAS; correct for both consumers; not a freshness oracle. |

No new REFUTED findings. The two hard refutations on these files remain V-002 (NEON scan4seq, G7) and V-003 (BIGFOOT
FIFO `at`, G7) — both cross-checked here at statement level (G6 reuse, P16f reuse); repros stay in wave-1
(`/tmp/gcaudit/`, documented in `G7-gc-adversarial.md`).

## 4. New lemmas / invariants

- **LEM-MINCORE-TAS**: mincore-cache bits record the post-action in-core state; the only consumers (prefault write,
  defrag source selection) use the answer as a perf hint whose false branch performs the action that makes the cached
  bit true. Torn cross-process reads are hint-only (with G7's benign-race note).
- **LEM-SCAN4SEQ-FB**: the fallback scanner checks each candidate position `[detent+1, len]` exactly once, in
  top-down order; unsigned no-wrap equality has no false positives for pgno-range data; the detent residue argument
  (`range − detent ≡ (len−seq) mod 4`) makes the exit-state case split exhaustive. `scan4range_checker` is its exact
  top-down twin ⇒ debug-assert agreement on sorted input.
- **LEM-STOCKPILE**: every successful `gc_reserve4stockpile` (gc_alloc_ex num=1 RESERVE) strictly grows
  `gc_stockpile` or consumes one finite GC id; exhaustion ⇒ NOTFOUND. Grounds P12f's while-loop termination (and the
  `cASSERT1(stockpile ≥ for_retired || err)` at :248).
- **LEM-ESCALATION**: the BIGFOOT entries-escalation loop terminates for all legal pagesizes because
  `maxgc_per_branch ≥ 13 > 1` (MDBX_MIN_PAGESIZE=256 ⇒ (256−16)/18 ≥ 13); the budget is a 1/13-ratio geometric sum.
- **INV-PREPFLAG**: `ctx->cursor.flags += / -= z_gcu_preparation` in gc_touch is an exact set/clear because no
  outer-cursor zone state carries bit 0x02 (cursor.h zone enum) and gc_touch cannot nest (no GC re-entry inside
  cursor_touch's spill path). Combined with `STATIC_ASSERT(ALLOC_RESERVE == z_gcu_preparation)`, is_reclaimable's
  inclusive-OR test makes the GC self-reclaim recursion guard airtight (G7 §1.4).
- **INV-EXACTLY-DEAD**: `ALLOC_EXACTLY` is `#if 0` (gc.h:61) — all `#if defined(ALLOC_EXACTLY)` blocks
  (gc-get.c:681-710, 726-734, 738-758) are compile-time dead; their audit is N-A.

## 5. KB keys written

- `fact:verification:gc-v7-statement-ledger` — this tranche's summary (lemmas LEM-MINCORE-TAS, LEM-SCAN4SEQ-FB,
  LEM-STOCKPILE, LEM-ESCALATION, INV-PREPFLAG, INV-EXACTLY-DEAD; nits F-V7-1, N-V7-2, N-V7-3; A1-A3 assumptions).
