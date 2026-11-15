# doc-additions-v3 — tranche V3 «page machine» (stream A, bottom-up)

Statement-level verification of `src/page-ops.c` (+page-ops.h), `src/page-get.c`,
`src/page-iov.c` (+page-iov.h), `src/page-split.c`, `src/node.c` (+node.h).
Reuses PAIR-N6/N7/N8/N9, INV-T1..T10, the PNL-slack/capacity/order and DPL-cap/order
lemmas without re-opening them. Invariant IDs below continue the
§8–12 (doc-additions-v2) / INV-C1–C5 (doc-additions-v2c) / INV-T1–T10
(doc-additions-v6-b) series; final §-numbers to be assigned at merge.
Executable evidence: `docs/verification/repro/v3-page-machine-probe.c`.

---

## 1. Proposed additions to `docs/engineering/libmdbx-invariants.md`

### INV-P1 (LEM-V3-1) — iov_page re-stamp is the on-disk txnid normalizer (closes the PAIR-N9 disk loop)
`page_dirty` stamps heap dirty pages with `front_txnid` (= txnid+1 for a malloc
writer), but the on-disk page stamp must equal the committing `txnid` (this is
what `coherency_check` enforces: root-page stamp == `trees[].mod_txnid`, and
`meta_validate` rejects `mod_txnid > meta.txnid`). The loop is closed by
`iov_page` (page-iov.c:150-153): in malloc mode **every** page of the current
txn's DPL carries `front = txnid+1 > txnid`, hence `is_shadowed`, hence takes
the re-stamp branch `dp->txnid = txn->txnid` before being queued for write
(nested-merged pages, stamped `parent.front+1`, are re-stamped the same way).
In WRITEMAP mode `front == txnid`, pages are `is_spilled`, take no re-stamp,
and the in-map stamp is already the committing txnid. Corollaries:
- on-disk page stamps ≡ committing txnid ≡ `mod_txnid` of the trees committed
  by that txn; readers' `check_page_header` tolerance (`page->txnid > front`
  only for parent-page lookups) and the `front >= maindb_leafpage_txnid`
  assert (table.c:118) are consistent with it;
- `page_unspill`'s `is_spilled` precondition is exactly "the map copy was
  written by spill (via `iov_page`) or is a WRITEMAP dirty page";
- `txn->txnid`-stamped (post-`iov_page`) pages never undergo further page ops
  in the txn (txn_write is the last mutation phase).

### INV-P2 (LEM-V3-8) — page_get_unchecked resolution order
For non-WRITEMAP write txns, `page_get_unchecked` resolves a pgno by walking
the txn chain child→parent and, **at each level**, testing the spill list
**before** the dirty list; readers/WRITEMAP skip the walk entirely (the map
bytes are always live). Correctness rests on: (a) within one txn a pgno is
never both spilled and dirty (spilling removes the DPL entry — mutual
exclusion, asserted at page-ops.c:496-514); (b) a child's lists are newer than
an ancestor's, so the nearest level wins; (c) `wr.repnl` is never consulted
(repnl pages are free, hence unreferenced by any live tree); (d) under
WRITEMAP dirty pages ARE map pages (AVOID_MSYNC changes only the persistence
transport, not the location of the live bytes). The final `r.page->pgno != pgno`
check is a defense against stale heap copies, not the primary lookup.

### INV-P3 (LEM-V3-2/3/4/5) — page_split split-point arithmetic is self-bounding
- **Dupfix reserve adjustment** (page-split.c:131-151): both branches
  (`wanna_reserve > left_space` / `> right_space`) provably keep
  `split_indx ∈ [leftmost_split, rightmost_split]` (proof: `wanna_reserve <
  (nkeys-1)/2` from the guard, `left_space/right_space ≥ minkeys-1`
  arithmetic; the inner ASSERTs at 140/142/146/150 hold structurally, not
  just by assertion). Machine-checked over the feasible domain
  (repro PROBE-2).
- **Tmp-path best-split scan** (241-268): `Σ(node+slot sizes of all old nodes)
  == page_used(env, mp)` because `page_used = (space − upper) + lower` counts
  exactly node bytes plus 2-byte entry slots; hence `right_size` (initialized
  to `newsize + page_used`) never underflows and bottoms at 0. `skew =
  (reserve_skew << 32) | split_skew < 2^63 < UINT64_MAX`, so the first
  qualifying candidate always initializes `best_split`; the loop terminates
  (`++i` every pass; `left_size` grows by > 0). The `skew >= best_skew` early
  break is a *quality* heuristic (unimodality of the lexicographic pair is not
  fully proved); a wrong early break can only select a suboptimal — never an
  invalid — split, and an overfull half is caught by `node_add_*`'s
  `lower > upper` → `MDBX_PAGE_FULL` → bailout → abort. Safety, not optimality,
  is the guarantee.
- **Tmp-return memcpy exactness** (465-470): `tmp_ki_copy`'s nodes are appended
  sequentially by `node_add_*`, so the last-added node (index n−1) sits at the
  lowest offset == final `tmp->upper` (stacking invariant) and the node region
  `[upper, space)` is contiguous; `memcpy(page_node(mp, n−1), page_node(tmp,
  n−1), space − tmp->upper)` therefore copies exactly the whole left-half node
  region with identical relative layout, after `mp->entries/lower/upper` were
  restored from tmp. `n ≥ 1` because the tmp path excludes `pure_left`
  (`split_indx ≥ minkeys ≥ 1`).
- **Move loop** (411-461): `ii` advances cyclically over `nkeys+1` slots and
  the loop re-terminates at `ii == split_indx` (finite cycle); the new key is
  positioned by `mc->ki[mc->top] = n` at `ii == newindx`; `mp` is only read —
  it is protected from spilling during the loop by the cursor-stash push
  (402-408, "avoid it spill-out": spill keeps pages referenced by any cursor
  stack), as are `sister` (`mc->pg[mc->top]`) and the parent.

### INV-P4 (LEM-V3-6) — page_check minimum-space formula
`PAGEHDRSZ + upper + nkeys·sizeof(node_t) + nkeys − 1 ≤ ps`
(page-get.c:133) is a sound necessary condition: keys are never empty
(`lmin ≥ 1`), so a keyed node occupies ≥ 9 bytes; a branch node 0 has `ks == 0`
(enforced at :198), giving exactly `9·nkeys − 1` for branches — the formula is
exact for branches (no false accepts, no false rejects). For leaves the formula
accepts exactly one extra region value, `region == 9·nkeys − 1` (reachable only
for odd `nkeys`, where `9·nkeys − 1` is even and thus parity-legal), and such a
region forces at least one zero-key node — caught per-node by the
`ksize ∉ [lmin, lmax]` check (:186-188, `lmin ≥ 1`). No uncovered false
negatives; machine-checked (repro PROBE-5).

### INV-P5 (LEM-V3-7) — is_tmp signature collision-freedom
`page_tmp_txnid_signature` is never equal to any page txnid that is not a tmp
signature: (a) checking formula — `a == b` would require
`52010·y ≡ x·65480 + 30319·52009 (mod 2³²)`; `gcd(52010, 2³²) = 2` and the RHS
is odd, so no solution exists and `potion ≠ 0` ("never zeroed" is provable);
(b) release formula — `pgno<<4 | pgno>>28 ≠ 0` for `pgno ≥ MIN_PAGENO = 3`
(either the high nibble or the shifted low bits are non-zero);
(c) `SAFE64_INVALID_THRESHOLD (0xFFFFffff00000000) > MAX_TXNID`, so the
signature range is strictly above every legal txnid and above
`INVALID_TXNID`-collision concerns for regular pages. Corollary (extends
PAIR-N9): a tmp page is always `is_shadowed`, hence accepted by page_check's
out-of-map gate exactly like a heap dirty page.

### INV-P6 (LEM-V3-9/10) — page_check out-of-map gate and its dead branch
The address gate (page-get.c:57-67) accepts exactly: in-map pages at
page-aligned offsets, in-map unaligned pages (which must carry P_SUBP), and
out-of-map pages that are `is_shadowed` (heap dirty pages of the current/nested
txn — the PAIR-N9 by-design identity) or sub-pages thereof (`flags_mask -=
P_SUBP` is precisely the subpage-of-heap allowance; the `PAGEHDRSZ+1` slack
admits a minimum sub-page header at the last allocated page's tail). Under
WRITEMAP any out-of-map address is rejected (map-only mode). `page_check` does
NOT check `page->txnid` (that is `check_page_header`'s job), N_TREE `tree_t`
payload contents (validated lazily on subtree traversal), large-page payload
bytes, or branch self-reference (a crafted self-loop is bounded by
`cursor_push`'s CURSOR_FULL guard). The frozen scatter-gather branch of
`page_kill` (page-ops.c:362-399) is dead code: its only caller passes
`status == modifiable`, and `is_modifiable ⟹ ¬is_frozen` by the front_txnid
machine — N-A (unreachability proved; the in-source comment documents it).

---

## 2. Proposed page-primitive contracts for `docs/engineering/module-interfaces.md`
(extends the page-ops.h / page-ops.c section of doc-additions-v2c)

### page-ops.c
- `page_new(mc, flags)` / `page_new_large(mc, npages)`: pre — tracked cursor,
  `DBI_DIRTY`, `MDBX_TXN_DIRTY`; the returned page is **already dirty**
  (`page_alloc_finalize` ran `page_dirty`: stamp = front, in DPL or
  WRITEMAP-tracked) with `pgno` stamped; post — `lower = 0, upper = ps−20`
  (new) or `P_LARGE, pages = npages` (large; `npages ≤ PAGELIST_LIMIT/4`);
  tree page counters incremented (outer tree mirrored for `z_inner`); large
  pages are never allocated for `z_inner` (assert). Errors leave no partial
  state beyond the aborted txn.
- `page_copy(dst, src, size)`: post — dst holds header + entries + nodes.
  P_DUPFIX: `20 + dupfix_ksize·numkeys` bytes; P_LARGE: all `size` bytes;
  otherwise when `upper − lower > 96` the free gap `[lower₈, upper₈)` is
  **skipped** (8-byte aligned boundaries; the dst gap stays zeroed because all
  dst are heap pages zeroed by `page_shadow_alloc`); the aligned path never
  triggers its `upper > copy_len` bailout (`upper₈ ≤ ps = size` for single
  pages). On a corrupted source: `bad_page` + `memset(dst, -1)`; **returns
  void** — the corruption is signalled only through logs/MDBX_TXN_ERROR by the
  subsequent checks of the copied page.
- `page_dirty(txn, mp, npages)`: pre — write txn; post — `mp->txnid = front`
  and (dirtylist present) a DPL entry consuming 1 dirtyroom, with the
  loose-purge rescue when `dirtyroom == 0` (one P_LOOSE page moved to repnl);
  `MDBX_TXN_FULL` (dirtyroom exhausted, no loose page) **releases the caller's
  page** when it is a heap page (`!AVOID_MSYNC || !WRITEMAP`) — WRITEMAP map
  pages are never released; WRITEMAP without dirtylist: only
  `writemap.dirty_npages += npages`. The LRU stamp (`size_t` at `page − 8`) is
  written only when `!AVOID_MSYNC || !WRITEMAP` — **map pages never receive a
  prefix stamp** (writing one would corrupt the previous page's tail);
  `page_shadow_alloc` reserves the prefix slot for heap pages.
- `page_touch(mc)` dispatcher: modifiable + no dirtylist (pure WRITEMAP) →
  success; modifiable subpage → success (host carries dirtiness); modifiable →
  `page_touch_modifiable` (DPL LRU stamp, or the AVOID_MSYNC-WRITEMAP
  not-in-DPL → `page_dirty` re-dirty path); else → `page_touch_unmodifiable`
  state machine: frozen → `gc_alloc_single` + `pnl_append(retired_pages)`
  (append AFTER successful allocation, BEFORE the parent pointer update; the
  V-003 growth mechanism — ordering verified) + parent/root fixup + cursor
  stack fixups (`mc->pg[mc->top] = np`, inner refresh, all tracked cursors
  pointing at mp); spilled → `page_unspill`; shadowed → shadow-clone +
  `page_dirty` ("insert a clone… don't touch dirtyroom"). `MDBX_MAP_FULL` /
  `MDBX_BACKLOG_DEPLETED` are returned **without** `MDBX_TXN_ERROR` only for
  the FREE_DBI cursor (`mc->dbi_state == txn->dbi_state`); every other failure
  sets `MDBX_TXN_ERROR`. The abandoned-but-allocated page on that early return
  is harmless: `gc_update` treats any error as commit-fatal and the txn is
  aborted (state discarded), so no page is leaked.
- `page_unspill(txn, mp)`: pre — `is_spilled(txn, mp)`, non-WRITEMAP; post —
  a heap copy dirty in THIS txn (stamp = front); a hit in an ancestor's spill
  list keeps that entry (page stays spilled until the child commits) and sets
  `P_SPILLED` on the copy; `MDBX_PROBLEM` (not found) is unreachable for legit
  stamps (`is_spilled ⟹ some ancestor's spill list holds it, or WRITEMAP —
  excluded by the pre`) and, unlike `page_touch_unmodifiable`'s own failures,
  does not set `MDBX_TXN_ERROR` (see finding V-034).
- `page_retire_ex(mc, pgno, mp, pageflags)`: status machine
  unknown/frozen/spilled/shadowed/modifiable per PAIR-N9; the `pageflags`-only
  fast paths require `pageflags ≠ 0` — large-page call sites pass `0`
  (tree-ops.c:44, tree-cutoff.c:199) and therefore always read the page, so
  `npages` is correct; `P_FROZEN`-flagged retirement asserts spill/DPL absence
  and goes straight to `retire`. Tail refund gate
  `pgno + npages == first_unallocated` (no uint32 overflow: any live large
  page satisfies `pgno + npages ≤ first_unallocated`); loose requires
  `npages == 1 ∧ suitable4loose ∧ (di ∨ !dirtylist)`; reclaim
  (`pnl_insert_span(repnl)` + MOST-tail `txn_refund`); spilled-with-parent-dirty
  → retire, spilled-own → reclaim, shadowed → reclaim (clone reuse), else
  retire. `page_kill` failure sets `MDBX_TXN_ERROR` but processing continues
  (wash/reclaim still run — the page is freed regardless; the error surfaces
  at the end).
- `page_shadow_alloc/release`: strict pair; the malloc block is
  `pgno2bytes(num) + sizeof(size_t)` with the page at `+8` (the LRU prefix);
  single-page pool (`dp_reserve_limit`); checking builds stamp `pgno = 0,
  flags = P_BAD` and zero the payload (NOMEMINIT honoured); `page_wash`'s
  `di == 0` tolerance is AVOID_MSYNC+WRITEMAP-only (non-WRITEMAP modifiable
  pages are always in the DPL).

### page-get.c
- `check_page_header(ILL, page, txn, front)`: ILL dispatch per PAIR-N8; the
  txnid tolerance clause (reject iff `page->txnid > front` AND
  (`> txn->front_txnid` ∨ `front < txn->txnid`)) permits parent-page lookups
  (`front = parent's stamp`) while rejecting pages newer than the current
  txn's visibility; lower/upper fast check for non-large, `pages`-field check
  for large (`1 ≤ npages < MAX_PAGENO/2`, `pgno + npages ≤ first_unallocated`).
- `page_check(mc, mp)`: see INV-P4/P6 for the checked set; per-node loop is
  bound-checked before every read (dupfix key bound, node offset ∈
  `[upper, ps−20]` with the node_del-underflow rationale, N_BIG pgno fetch via
  `page_get_large(..., mp->txnid)` + `largechunk_npages` cross-check skipped
  under `z_retiring`, N_DUP sub-page full sub-validation incl. its own offset
  bound `dsize − 20`); the dupfix `lmin/lmax` re-pin mutates the cursor's
  shared `clc` (documented side effect of validation);
  after a failed `leaf2_ksize != tree->dupfix_size` check the per-key loop
  still runs with mixed sizes — safe because each iteration re-checks
  `end_of_page < key + leaf2_ksize` before any read.
- `page_get_unchecked`: INV-P2; `pgno ≥ first_unallocated → MDBX_PAGE_NOTFOUND`
  (no overflow); callers turn any error into `MDBX_TXN_ERROR`
  (page_get_inline:510). `MDBX_DISABLE_VALIDATION` skips
  `check_page_header` (documented LMDB-benchmark mode).

### page-iov.c / page-iov.h
- `iov_init(txn, ctx, items, npages, fd, check_coherence)`: post —
  `coherency_timestamp = UINT64_MAX` (skip) iff `!check_coherence` and the
  incoherence counter is zero; ring prepared for `max(items,32)` slots and
  `pgno_ceil2sp_bytes(npages)` bytes; `flush_begin/end` reset to sentinels
  (`MAX_PAGENO`/`MIN_PAGENO`) under `MDBX_NEED_WRITTEN_RANGE`.
- `iov_page(txn, ctx, dp, npages)`: pre — `ctx->err == SUCCESS`,
  `MIN_PAGENO ≤ dp->pgno < first_unallocated`, `is_modifiable(txn, dp)`;
  INV-P1 re-stamp; `MDBX_RESULT_TRUE` (ring full) → `iov_write` (which
  completes+resets) → single re-add (a second RESULT_TRUE is impossible after
  a successful drain — `osal_ioring_reset` restores all slots; the branch is
  defensive); on any failure `iov_complete` is invoked (idempotent — the walk
  on a reset ring is a no-op) and the error is propagated in `ctx->err` and as
  the return value; the flush range is extended only on the success path
  (error paths leave it — the commit fails anyway).
- `iov_write(ctx)`: `osal_ioring_write` (pwrite/pwritev with full
  partial-write loops and MAX_IO_BYTES chunking — see finding V-037) +
  `iov_complete` (walk → coherency check → `page_shadow_release` per page /
  per large chunk). Multi-page release walk terminates (`bytes` decreases by
  ≥ ps per pass); the coherency `memcmp` retry loop is bounded by
  `coherency_timeout` (0.1 s) → `MDBX_PROBLEM`.
- `iov_empty` — `osal_ioring_used == 0`.

### node.c / node.h
- Node geometry: `node_size_len(ks, ds) = 8 + EVEN_CEIL(ks + ds)` — key and
  data are evened **together**; matches `node_del`'s `EVEN_CEIL(hole_size)` and
  the page_check minimal-space formula (INV-P4).
- `node_add_branch`: capacity check **before** the entries shift;
  `memcpy_checklen` guards zero-length keys (`MDBX_CHECK_MEMZEROLEN` builds);
  bounds follow from `branch_bytes ≤ upper` (keys ≤ MDBX_MAXKEYLEN so
  `node_set_ks`'s `INT16_MAX` assert cannot fire).
- `node_add_leaf`: the N_BIG-pre-set path (`sizeof(pgno_t)` memcpy from
  `data->iov_base`) relies on the internal caller passing a pgno-sized buffer
  (PROVED-A); the large-page path rejects DUPSORT/N_DUP/N_TREE targets;
  RESERVE hands the caller `node_data` / the large-page payload (bounded by
  `largechunk_npages`); the entries shift happens **before** the
  `lower > upper` check (PAGE_FULL leaves the page shifted — abort-only
  recovery; checking builds assert `page_room` beforehand) — see V-035.
- `node_add_dupfix`: capacity condition `lower > upper` ⟺
  `(nkeys+1)·ksize > space` — exactly the physical key-data bound
  `PAGEHDRSZ + (nkeys+1)·ksize ≤ ps` (dupfix entry slots are counters, not
  stored bytes: the +2 shifts of `lower` and `upper` cancel; model
  `upper_n = space − n·(ksize−2)`, `lower_n = 2n`). The parity invariant
  `((ksize & nkeys) ^ upper) & 1 == 0` holds for even and odd `dupfix_size`
  (`upper ≡ n·ksize mod 2`). Machine-checked (repro PROBE-7).
- `node_del`: dupfix mirror of add; usual path — hole compaction with
  `memmove(base + hole_size, base, hole_offset − upper)`; the
  `hole_offset ≥ upper` guard is **cASSERT-only** and its primary guard
  (page_check offset check) runs only under MDBX_VALIDATION — see V-035 for
  the trust-model asymmetry with `subpage_geometry_ok`.
- `node_shrink`: delta = `EVEN_FLOOR(page_room(sp))`; the host memmove moves
  `[host_upper, sp + len)` up by delta (len = PAGEHDRSZ for non-dupfix subps,
  nsize for dupfix); the sub-page payload stays at its absolute offsets and is
  re-based via `xp->entries[i] = sp->entries[i] − delta` and `sp->upper −= delta`
  — the written header is carried by the memmove; entries+delta wrap is
  uint16-checked only by ASSERT (V-035); returns the node's new address.
- `node_read_bigdata`: `page_get_large` + `pages ≥ largechunk_npages(ds)`
  (skipped under `MDBX_DISABLE_VALIDATION`); `data->iov_base` = large-page
  payload; no upper bound beyond capacity (the logical `ds` comes from the
  node).

---

## 3. Proposed additions to `docs/engineering/test-coverage.md`

1. **V-036 (root-split stack bound, F1)**: crafted-meta test — build a DB,
   patch a valid meta to `height = CURSOR_STACK_SIZE−1` with a matching
   full-root tree, re-open (meta_validate permits height ≤ CURSOR_STACK_SIZE−1)
   and force one more root split; under ASAN the m3 shift writes
   `pg[CURSOR_STACK_SIZE]` (one past `pg[]`). Also add the cheap fix
   (bound the shift by `m3->top`) or a `tree->height < CURSOR_STACK_SIZE`
   guard before the root split, then a checking-build regression that a
   committed tree never exceeds `CURSOR_STACK_SIZE−1` height.
2. **page_split boundary matrix**: with `split_reserve_dot16 > 0` and
   DUPFIXED tables of both parities of `dupfix_size`, exercise
   `newindx ∈ {0, split_indx−1, split_indx, split_indx+1, nkeys}` and verify
   post-split pages under `MDBX_VALIDATION` (covers LEM-V3-2/3 and the
   `MDBX_SPLIT_REPLACE` paths from cursor-mut.c:665 / tree-ops.c:930).
3. **Tmp-path big-node split**: leaf with near-full page + a huge new value
   (`newsize ≥ dim_bytes`) forcing the byte-exact best-split scan; assert the
   resulting halves pass `page_check` and `cursor_validate_updating` (already
   checked in CHECKS2 — add a release-mode validation pass).
4. **Nested txn dispatch matrix**: nested txn touching (a) a parent-dirtied
   page (shadowed → clone), (b) a parent-spilled page (is_spilled → unspill
   from the parent's list, `P_SPILLED` copy), (c) a nested-dirtied page at
   basal commit (`iov_page` re-stamp) — locks INV-P1/PAIR-N9.
5. **iov_page ring-full retry**: set `dp_limit` below the dirty page count so
   `osal_ioring_add` returns `MDBX_RESULT_TRUE` mid-commit; assert commit
   success and `flush_begin/end` coverage; add a stub-write failure injection
   on the retry add (error must propagate — the V-series note at
   page-iov.c:167-171).
6. **V-037 (subpage thresholds)**: `MDBX_opt_subpage_room_threshold = 65535`
   + `MDBX_opt_subpage_reserve_limit = 65535` on a small pagesize; run a
   checking build (the unsatisfiable `eASSERT` in
   `recalculate_subpage_thresholds`) and a release build (behaviour
   degradation only) — fix shape: make the assert conditional on the branch
   taken.
7. **Large-page single-write > MAX_IO_BYTES**: valsize near `valsize_max`
   with `MDBX_ENABLE_PGET_STAT`-style instrumentation disabled; verify
   `osal_pwrite` partial-write looping covers the oversized single item on a
   release build (V-037 family) — or clamp `iov_page`'s npages.
8. **Crafted-page unit tests under MDBX_VALIDATION** (V-035 family): node
   offset < upper (node_del memmove), sub-page entries+delta wrap, dupfix
   ksize mismatch (`page_check`'s loop-after-bad_page path), branch
   self-reference (bounded by CURSOR_FULL).

---

## 4. Findings (V-ids continue the global registry)

| V-id | file:line | severity | description |
|---|---|---|---|
| V-036 | page-split.c:507-519 | low (UB; crafted-input reachability) | The root-split adjustment of other cursors shifts levels `0..root_split_height` where `root_split_height` is the **old tree height** (`mc->tree->height++` post-increment), not the individual cursor's `top`. For any legal tree `m3->top ≤ height−1`, so the extra levels are garbage-but-in-bounds — except at the maximal height permitted by `meta_validate` (`height ≤ CURSOR_STACK_SIZE−1`): the loop's last iteration writes `m3->pg[height+1] == m3->pg[CURSOR_STACK_SIZE]`, one past `pg[]` (in practice aliases `ki[0]`, which the very next statement overwrites — no lasting corruption, but formally UB and layout-dependent). Reachable only from a crafted meta of maximal height followed by one more root split; the split also commits `height = CURSOR_STACK_SIZE`, which the next `meta_validate` rejects → the DB becomes unopenable (availability loss in an already-adversarial scenario). Legitimate trees cannot reach the bound (root-split growth requires ~`branch_maxkeys^(h−1)` leaf splits). Fix shape: bound the loop by `m3->top` (`for (k = m3->top; k >= 0; k--)`) — strictly correct and cheaper — or guard the root split at `height ≥ CURSOR_STACK_SIZE−1`. Machine-checked: repro PROBE-1. |
| V-037 | page-ops.c:770-786; page-iov.c:157 + osal-ioring | low (checking-build) / info | (a) `recalculate_subpage_thresholds`' final `eASSERT(prereq >= threshold + limit)` is unsatisfiable in its own first branch (`prereq = page_space` when `threshold + limit > page_space`) — user-settable via `MDBX_opt_subpage_room_threshold/reserve_limit` up to 65535 with `leaf_nodemax > page_space/2`; release builds degrade safely (sub-page reserves disabled). (b) A single large-page write can exceed `MAX_IO_BYTES` (0x3f000000 POSIX): `largechunk_npages(valsize_max)·ps` is bounded by `valsize_max` caps (`PAGELIST_LIMIT/4` pages, mmap_limit/2) but the `ASSERT(bytes ≤ MAX_IO_BYTES)` in `osal_ioring_add` is checking-only; release-safe because `osal_pwrite` clamps to `MAX_IO_BYTES` and loops partial writes, and the sgv-merge path caps merges explicitly. Hygiene: enforce or document the contract. |
| V-035 | node.c:210-215, 282-287, 136-145; page-get.c:163 | info (trust model) | Crafted-page hardening asymmetry: `node_del`'s `hole_offset ≥ upper` guard (defense against a `memmove` length underflow to ~SIZE_MAX) is cASSERT-only, and its "primary guard" (the entries-offset check in `page_check`) executes only under `MDBX_VALIDATION` — in default builds a crafted main-page node offset below `upper` reaches the underflowed memmove. The analogous sub-page vectors were hardened with the O(1) hot-path `subpage_geometry_ok` (issues #57/#176); main pages were not. Same family: `node_shrink`'s `entries[i] + delta` uint16 wrap (ASSERT-only), `node_add_leaf`'s shift-before-capacity-check (PAGE_FULL leaves shifted entries; abort-only recovery). Within the documented "DB contents are trusted; use MDBX_VALIDATION for untrusted data" position (node.h:84-109), but inconsistent with the sub-page precedent — either extend `subpage_geometry_ok`-style checks to the main-page `hole_offset` case or record the asymmetry explicitly. |
| V-034 | page-ops.c:128; cursor-mut.c:335/342; gc-put.c:1297 | info | Error-propagation hygiene: `page_unspill`'s `MDBX_PROBLEM` return (spill entry not found) and the "shadowed large page without parent" `MDBX_PROBLEM` (cursor-mut.c:342) do **not** set `MDBX_TXN_ERROR`, whereas `page_touch_unmodifiable` sets it on every failure path. All four sites are unreachable for legit stamps (`is_spilled ⟹ some ancestor's spill list holds the page`; a shadowed page without a parent violates PAIR-N9), so the inconsistency is latent-only — but a future caller of `page_unspill` would inherit the gap. Fix shape: set `MDBX_TXN_ERROR` inside `page_unspill`'s PROBLEM branch. |

Established lemmas (registered in the KB as fact:verification:*):
LEM-V3-1 (iov-page disk-stamp normalizer, INV-P1), LEM-V3-2 (dupfix reserve
bounds), LEM-V3-3 (tmp scan accounting/termination), LEM-V3-4 (tmp-return
memcpy exactness via the node-stacking invariant), LEM-V3-5 (move-loop
termination + stash spill-protection), LEM-V3-6 (page_check minimum-space
formula), LEM-V3-7 (is_tmp collision-freedom), LEM-V3-8
(page_get_unchecked resolution order), LEM-V3-9 (out-of-map gate), LEM-V3-10
(page_kill frozen branch dead).
