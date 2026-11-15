# doc-additions-v4 — tranche V4 «tree» (stream A, bottom-up)

Statement-level verification of `src/tree-search.c`, `src/tree-ops.c`,
`src/tree-cutoff.c`, `src/walk.c` (+walk.h), `src/dbi.c` (+dbi.h),
`src/table.c`. Reuses PAIR-N6/N7/N9, LEM-V3-1..10 (page machine), INV-T2
(cursor_eot restore), V-020 (delta ≤ save_top assert-only), V-038
(BUNCHES_REMOVAL build break — scope re-verified, see §7), INV-CC1 (walk
containment via z_pagecheck), INV-CC2 (dbi seq-last), the dbi state machine and
tree-ops contracts (fact:verification:tree-ops-contracts,
fact:verification:dbi-state-machine) without re-opening them. Invariant IDs
continue the INV-P/INV-T/INV-C series (doc-additions-v2/v2c/v3/v3c/v5-b/v6-b);
final §-numbers to be assigned at merge. No REFUTED verdicts in this tranche —
no reproducers required; every claim below was checked statement-by-statement
against the sources at commit 9ad900f4 (branch verification/master-audit).

---

## 1. Proposed additions to `docs/engineering/libmdbx-invariants.md`

### INV-V4-1 (LEM-V4-1) — the branchless search cycles are exact lower_bounds
`BINARY_BRANCHLESS_SEARCH_CYCLE_BEGIN/END` (tree-search.c:158-167), used by
every `SEARCH_FOLIAGE_*` and `SEARCH_BRANCH_*` instantiation, implements a
branchless lower_bound with invariant **answer ∈ [lo, lo+scope]**:

- probe `it = lo + half`, `half = (scope-1)>>1`, `adjust = (scope-1)&1`;
- `node_key < key` (cmp<0): `lo += half+1`, `scope = half+adjust` — the new
  range is exactly `[it+1, old lo+scope-1]`;
- `node_key >= key` (cmp≥0): `scope = half` — the answer may be `it` itself,
  which stays at the inclusive upper edge `lo+scope`;
- `scope` strictly decreases (`1→0`, `2→1→0`, `n→ceil((n-1)/2)`), so the
  do-while terminates; the entry guard `hi < lo` (nkeys ≤ lo, incl. the
  `lo = flags & P_BRANCH` skip of branch node 0) guarantees `scope ≥ 1` on the
  first iteration, so the "probe with half = -1" hazard is unreachable;
- after exit `it += cmp & 1` (with `cmp` already sign-shifted to `{-1,0}` by
  the END macro) restores `it == lo == answer`; `mc->ki[top]` ends in
  `[0, nkeys]` (`nkeys` = past-end only for foliage searches with no larger
  key), `ret.node ∈ {nullptr} ∪ {page_node(i): i < nkeys}`.

The `exact` flag is **conservative**: when the loop exits via a `cmp < 0` move
onto an index that was never probed, an exactly-equal key at the answer index
is reported `exact = false` with `node` pointing at it. Consumers must (and
the verified ones do) re-compare rather than trust `exact`. `SEARCH_BRANCH`
additionally clamps its return to `[0, nkeys-1]` (`it = lo-1`) and never
reports `exact` — the follow-subtree contract, see INV-V4-2.

### INV-V4-2 (LEM-V4-2) — branch-page separator convention
For a branch page, **node `i ≥ 1` stores the minimum key of subtree `i`**;
**node 0's key is a don't-care placeholder**. Consequences that all structural
code maintains and reviewers must preserve:

- `search_branch` returns the largest `i ∈ [1, nkeys-1]` with
  `node_key(i) ≤ key`, else 0 — following that subtree is exact because
  subtree `i`'s keys live in `[node_key(i), node_key(i+1))`;
- node 0's key is skipped by search (`lo = 1`), re-nulled by
  `tree_propagate_nullkey` (proto.h:172, a zero-length
  `tree_propagate_key`), and refreshed to the true minimum whenever an
  operation shifts a node from position 0 to position ≥ 1 — this is why
  `node_move`'s `cdst->ki == 0` path fetches the lowest key under dst
  (tree-ops.c:161-208) **before** `node_add_branch(cdst, 0, …)`: the refreshed
  key lands at position 1 where it must be the true minimum; and why the
  `csrc->ki == 0` path fetches the lowest key under src instead of using the
  placeholder (tree-ops.c:129-159);
- `page_merge`'s branch case likewise replaces node 0's key with the fetched
  minimum (tree-cutoff n/a; tree-ops.c:447-478), and the space pre-check
  `src_used − node_ks(srcnode) + key.iov_len` is exact modulo a ≤1-byte
  EVEN_CEIL shortfall of an odd `dupfix_size`, backstopped by
  `node_add_branch`'s PAGE_FULL → `MDBX_RESULT_TRUE` retry;
- `node_move`'s room pre-check `branch_size(key4move) + delta` is EXACT:
  `branch_size = NODESIZE + EVEN(klen) + sizeof(indx_t)` (node.h/cogs.h:158)
  already includes the entry slot, and `delta =
  EVEN_CEIL(key.iov_len) − EVEN_CEIL(node_ks(pdst node 0))` accounts the
  position-0 key replacement either way (placeholder or stale real key).

### INV-V4-3 (LEM-V4-3) — frozen parent ⇒ frozen children
`page_touch_unmodifiable`'s CoW path and `page_unspill` must update the parent
branch node's child pgno (page-ops.c:178-186), which requires the parent to be
modifiable. Contrapositive: **if the parent page is frozen (not dirtied in
this txn), none of its children were touched/unspilled in this txn**. This
validates both:

- `tree_drop`'s branch-level retire pagetype inference
  (tree-ops.c:65-66): `P_FROZEN` is decided by `is_frozen(txn, mp)` on the
  **parent** branch page and then asserted onto each child
  `page_retire_ex(mc, pgno, nullptr, pagetype)` without reading the child;
- `tree_cutoff_twig`'s whole-tree fast path (tree-cutoff.c:120-127):
  `parent_txnid < mc->txn->txnid` is exactly "frozen" — DPL/shadow pages are
  always stamped `≥ front_txnid` (LEM-V3-1), so a stamp `< txnid` cannot be a
  this-txn page; and `tbl_root_txnid(mc->txn, dbi)` is a **conservative
  proxy** for the leaf-node page's frozenness: a dirty leaf implies a dirty
  root (INV-V4-3 chain), so a frozen root guarantees frozen descendants;
  overestimates only *reject* the fast path.

### INV-V4-4 (LEM-V4-4) — walk_pgno byte accounting is exact per page shape
`walk_pgno`'s per-page decomposition `(payload, header, unused+align)` sums to
the page size for every legal shape:

- **branch**: `PAGEHDRSZ + lower + Σ NODESIZE` header, `Σ ks` payload,
  `Σ (ks & 1)` align — branch pgno lives inside `NODESIZE`
  (`branch_size = NODESIZE + EVEN(ks) + sizeof(indx_t)`, cogs.h:158);
- **leaf**: per node `NODESIZE + ks + ds + ((ks+ds) & 1)` — exact;
  `N_BIG` adds `sizeof(pgno_t)` payload (node footprint
  `NODESIZE + EVEN(ks) + pgno_t`); `N_TREE` adds `ds` to header;
- **dupfix leaf**: header = `PAGEHDRSZ`, payload = `nkeys·dupfix_ksize`,
  unused = `page_room` — the `upper/lower` pair is virtual bookkeeping here
  (records physically start at the payload), and
  `room = ps − PAGEHDRSZ − nkeys·ksize` so the three sum to `ps`;
- **N_DUP sub-page**: `ds = PAGEHDRSZ + sp->upper =
  (PAGEHDRSZ + sp->lower) + (sp->upper − sp->lower) = subheader + subunused`,
  and `subpayload + subalign = Σ EVEN(subnode)` completes the sub-page node
  area — total `ds` exact; **dupfix sub-page** analogous with
  `subheader = PAGEHDRSZ`, `subunused = ds − PAGEHDRSZ − nkeys·ksize`;
- **large**: `over_unused = npages·ps − ds − PAGEHDRSZ`, one visitor call for
  the whole span (the revisit bitmap marks the base pgno only — the chk
  pagemap marks the full span, see INV-C1).

Per-page `nwalked` counts pgno *references*, and `walk_limit =
first_unallocated` bounds them; the revisit bitmap is capped at 2²⁴ pages
(2 MiB) — pages above the cap are not marked (best-effort revisit detection
for large files, by design; `nwalked` still bounds the total). The bitmap
persists across the GC and MAIN `walk_tbl` invocations, which is correct
because a valid file's trees are page-disjoint (a cross-tree double reference
is corruption).

### INV-V4-5 — cutoff_zikkurat region discipline (BUNCHES_REMOVAL)
Within one level pass, the cut region is `[ki_begin, ki_end−1]` with
`ki_begin = 1 + begin->ki[level]` (node 0 skipped — placeholder key, no
separator maintenance needed — and begin's subtree preserved); the do-loop
keeps `page_numkeys(mp) > 2` (branch minkeys) and stops at `ki_begin`;
post-loop `mc->ki[level]` points at the first surviving node. The other-cursor
fixup partitions exactly: `ki < mc->ki[level]` skip (below region, incl.
begin — hence `ASSERT(m3 != begin)` holds), `ki ∈ [ki_begin, ki_end−1]`
truncate to `top_and_stash = (uint16_t)level` + reposition
(sibling_right, NOTFOUND → `ki = nkeys−1` + `Z_LAST`, else `Z_FIRST`) +
`z_after_delete`, `ki ≥ ki_end` → `ki −= dropped`. Termination per level:
`tree->items < 4` bailout, `numkeys > 2` key floor, and
`cursor_sibling_left` + the begin-boundary check (which may stop early on a
meaningless cross-page `ki[level]` comparison — conservative, the residual
rows are then deleted by cutoff_storey's leaf loop). The dozer couple is
registered at tree-cutoff.c:238-239 and unregistered at :356 on every exit
path; `MDBX_IS_ERROR(err)` ⇒ `MDBX_TXN_ERROR` (a partial cut is not undoable).

### INV-V4-6 — cursor combo_state packing (read side of PAIR-N6)
`combo_state` (int32) overlays `flags` (int16, low half),
`top_and_stash` (int16) where `top`/`stash` are its low/high int8 halves
(internals.h:314-359). `combo_state |= z_poor_mark` (−64, bits 6/7 of every
byte) is the documented one-store reset: `top = −1`, `stash = −1`,
`flags |= eof_hard|hollow`; `be_poor` additionally kills the nested tree
(`tree->root = 0` for inner cursors — the nested-tree "empty/never-initialized"
encoding, distinct from the outer `P_INVALID`). `cursor_push` writes
`top_and_stash = new_top` which clears `stash` by construction; the packing is
the read side of PAIR-N6's one-compare state gates.

---

## 2. Tree-primitive contracts (proposed for `module-interfaces.md`)

### tree_search (tree-search.c:27) — `int tree_search(mc, key, flags)`
- **Pre**: `mc` tracked/pure; `mc->tree` bound; flags ∈ {0, Z_MODIFY,
  Z_ROOTONLY, Z_FIRST, Z_LAST} (bitwise composable except Z_FIRST|Z_LAST).
- **Post (success)**: stack reset to the root (`top_and_stash = 0`, flags
  preserved), descended to the leaf holding/lower-bounding `key`;
  `ki[top] ∈ [0, numkeys)`; leaf type validated via `check_leaf_type`
  (skipped under `MDBX_DISABLE_VALIDATION`); with `Z_ROOTONLY` stops at the
  root; with `Z_FIRST|Z_LAST` positions at the extreme leaf
  (ki = numkeys−1 legal on the last).
- **Post (empty tree)**: `MDBX_NOTFOUND`, cursor already poor
  (`is_poor(mc)` asserted), stack untouched.
- **Errors**: `MDBX_BAD_TXN` / stale-dbi refresh failure / page_get failure /
  `MDBX_CURSOR_FULL` (stack) / `MDBX_CORRUPTED` (leaf type) — all with
  `be_poor(mc)`; page_get failures are txn-fatal.
- **Depth**: descent adds ≤ height levels; `height < CURSOR_STACK_SIZE`
  (meta_validate/tbl_fetch:111), `cursor_push` re-checks and fails poor.

### tree_search_branch_configure / tree_search_foliage_configure
- **Pre**: `mc->clc->k.cmp != nullptr`; for foliage: `mc->pg[mc->top]` is a
  leaf/branch of the expected type (dispatch checks `is_inner` +
  `MDBX_DUPFIXED` for dupfix variants; the ordinal fast paths additionally
  require `MDBX_VALIDATION` off and `lmin == lmax == keylen ∈ {4,8}`).
- **Post**: result per INV-V4-1; the chosen function is cached in
  `clc->k.search_foliage` / `search_branch` (per-dbi kvx, invalidated by
  `clc_reset_methods` — tbl_setup/tbl_bind).
- **Side effect**: the DUPFIXED inner dispatch may narrow
  `clc->k.lmin/lmax` to `dupfix_size` (tree-search.c:432-435) — sound
  because every key of a DUPFIXED tree has that length (enforced by
  `cursor_put_checklen`/tbl_setup:30-35).

### tree_deepen_edge / tree_deepen_lowest (tree-search.c:112/10)
- **Pre (edge)**: `!is_poor(mc)`, `mc->pg[mc->top]` valid, flags Z_FIRST/Z_LAST
  (+Z_MODIFY); the pushed `ki` per level is `0` or `numkeys−1` of the child.
- **Pre (lowest)**: `top ≥ 0`, `pg[top]` is branch with ≥ 1 key
  (callers: node_move/page_merge on non-empty subtrees); pushes down to the
  leftmost leaf, `ki = 0` on every pushed level.
- **Post**: leaf reached and type-checked; **errors are be_poor'd by
  deepen_edge but NOT by deepen_lowest** — callers (node_move:133-136,
  page_merge:455-458) treat any non-SUCCESS as txn-fatal and discard the
  cursor state.

### node_move (tree-ops.c:104) — static, called by tree_rebalance
- **Pre**: `psrc/pdst` same page type, same tree, `csrc->top == cdst->top`;
  `csrc->ki[top]`, `cdst->ki[top]` in `[0, numkeys)`; `fromleft` ⇒ the moved
  node is `csrc`'s LAST node (`ki = numkeys−1`), else node 0.
- **Post (success)**: the node moved; `key4move` = true min of the moved
  subtree (placeholder replaced when `csrc->ki == 0`); separators maintained
  per INV-V4-2 on both sides incl. parent propagation
  (`tree_propagate_key`/`tree_propagate_nullkey`, the latter cannot fail —
  shrink-only path); other-cursor fixups complete because "last node" /
  "insert at 0" bounds the affected index ranges (tree-ops.c:276-329);
  `MDBX_RESULT_TRUE` ⇒ nothing mutated (pre-checks precede page_touch).
- **Errors**: `MDBX_RESULT_TRUE` (no room, clean) or txn-fatal with
  `MDBX_TXN_ERROR` (type mismatch bailout).

### page_merge (tree-ops.c:402) — static, called by tree_rebalance
- **Pre**: `csrc != cdst`, tracked, same type/tree/clc, `top > 0`
  (non-root), `csrc` is the RIGHT sibling of `cdst` (append-at-end preserves
  order in both call directions); GC callers exempt from the room assert.
- **Post (success)**: all src nodes appended at `ii = dst_nkeys + i`;
  src's parent node deleted (enroot/node_del/undo-enroot pairing
  tree-ops.c:518-528, undo skipped only on txn-fatal); src retired;
  other cursors relocated (`pg[top] = pdst`, `ki += dst_nkeys`,
  `ki[top−1] = cdst->ki[top−1]` post-deletion) or parent-slot decremented;
  then `tree_rebalance(cdst)` with
  `cdst->top_and_stash = save_top − delta` (V-020 site) or the leaf
  `stash = 0` exit — **both exits yield top = the (possibly re-leveled)
  position with stash 0**; `MDBX_RESULT_TRUE` from the inner merge attempt is
  consumed by the caller's retry loop, never escapes as success.
- **Post (dst cursor)**: `mc->pg[mc->top]` valid; callers re-read
  `mc->pg[mc->top]/ki` (tree_rebalance does).

### tree_rebalance (tree-ops.c:593)
- **Pre**: tracked cursor, `top ≥ 0`, page modifiable; `stash` semantics
  per cursor.h (tracked = top+stash+1 pages).
- **Post**: the page at `mc->top` satisfies minkeys (or is the root:
  collapse/empty handling) or `MDBX_SUCCESS` without changes when room is
  acceptable; success leaves `top_and_stash` consistent with any collapse
  (root-collapse shift covers `top+stash`, never the on-disk height —
  issue-#63 hardening, tree-ops.c:678-697); escalation ladder
  (minimize_waf → room_threshold 0 → involve clean pages) terminates after
  ≤ 3 `goto retry` steps; unreachable-merge ⇒ `MDBX_PROBLEM` (txn error).
- **Note**: the root-collapse does not decrement `mc->top_and_stash`
  (leaving one duplicate tail entry `pg[top+stash]`); harmless in release —
  every consumer before the encoding reset (page_merge:587-588 or the leaf
  `stash = 0`) rewrites the packed value; debug-only asserts may read the
  duplicate.

### tree_drop (tree-ops.c:13) — BUNCHES_REMOVAL=1 path: tree_cutoff_twig
- **Post (success)**: all pages of `mc->tree` retired bottom-up
  (children before parents, root last via page_retire(pg[0])); capacity
  reserved up-front (`pnl_need`, branch+leaf+large counters — exact under
  trust model A2); nested DUPSORT trees dropped recursively with their own
  reservations; cursor left poor by the caller wrapper (`be_poor` at :100).
- **Post (partial failure)**: txn-fatal; consumers set `MDBX_TXN_ERROR`
  (cursor-mut.c:1034→fail:, table.c:166-170); other cursors are NOT adjusted
  inside — caller-side invalidation contract (cursor_del
  inner_gone_unconditional; tbl_purge be_poor-all).
- **Legacy path (BUNCHES_REMOVAL=0)**: compiles after renaming
  `cursor_pop` → `cursor_eject` at tree-ops.c:25/:87 (see §7); walk order
  bottom-up with the saved `stack[]` restore on popup; the leaf-scan skip
  `!(may_have_subtrees | large_pages)` and the post-N_BIG
  `goto popup` exploit `page_retire_ex`'s live decrement of
  `tree->large_pages` (page-ops.c:524-528).

### tree_cutoff_twig (tree-cutoff.c:112, BUNCHES_REMOVAL=1)
- **Pre**: `deep ≥ 1` (1 = root), `parent_txnid` = the parent page's stamp;
  recursion depth guarded `deep > CURSOR_STACK_SIZE → MDBX_CORRUPTED`
  (crafted-cycle defense).
- **Post (success)**: every page of the subtree retired (bottom-up, page
  itself last); with `whole_tree` no item accounting; with `whole_tree=false`
  `items −= numkeys(leaf) + Σ(inner items − 1)` per dupsort node — consistent
  with cursor_del's row-counting convention
  (`items -= inner−1` at cursor-mut.c:949 + `del_key: items -= 1`), i.e.
  outer `tree->items` counts ROWS (keys×dups) for dupsort tables;
  leaf reads skipped entirely when the fast path applies (no pagecheck,
  deep == height, no large pages, frozen parent, no subcur, not MAIN_DBI);
  push/eject pair (tree-cutoff.c:171/:197) balanced for both N_TREE and
  sub-page nodes.

### tree_cutoff_range / cutoff_storey / cutoff_zikkurat (tree-cutoff.c:510/366/210)
- **Pre**: begin/end same txn/clc/top, both pointed, `begin < end` (or equal
  with end_including handled earlier); `level < begin->top` for zikkurat
  (caller-gated).
- **Post (success)**: rows in `[begin, end]` deleted (`end_including`
  selects end's row); cursors re-anchored (zikkurat may set
  `z_after_delete` ⇒ `MDBX_RESULT_TRUE` to the caller); for dupsort ranges
  the boundary hives are trimmed (end's head / begin's tail), the middle
  keys deleted wholesale, and the final outer-driven `cursor_del(outer, 0)`
  finishes the last hive (handling the sub-page → plain-node collapse that
  inner-driven deletions must not perform — hence the while loop's
  `items == 2` stop). Items accounting per INV-V4-2's row-counting
  convention (`outer->tree->items -= dups − remaining`).
- **Post (error)**: `MDBX_TXN_ERROR` set (partial cut not undoable).

### walk_pages / walk_tbl / walk_pgno (walk.c:366/336/68)
- **Pre**: `check_txn(txn, MDBX_TXN_BLOCKED)`; `walk_func` must bound the
  reported `deep` (all current consumers do: chk 42, api-copy deep_limit,
  defrag walk_stack cap) — the C-stack recursion has no structural depth
  bound beyond the visitor (see §6 F-V4-4).
- **Post**: every page of the GC and MAIN trees (unless dont_walk_*) visited
  exactly once with the visitor contract (pgno, npages, deep, table, psize,
  type, txnid, err, nentries, payload, header, unused+align, parent) —
  sizes exact per INV-V4-4; revisit/limit guards per INV-V4-4; walk cursors
  armed `z_pagecheck` unconditionally (INV-C1) and chained via
  `ctx->cursor` (restored on every walk_tbl exit, incl. error).

---

## 3. dbi/table primitive contracts (proposed for `module-interfaces.md`)

### dbi_open (dbi.c:645) — lock-free fastpath + locked slowpath
- **Pre**: `dbi` out-pointer; writable txn for CREATE; name ≤
  `leaf_nodemax − NODESIZE − sizeof(tree_t)`.
- **Post**: `*dbi` bound with `DBI_LINDO|DBI_VALID(+STALE)` in the txn;
  env slot published (`dbs_flags = flags|DB_VALID`, name buffer owned by
  `kvs[]` with a `defer_free_item_t` header); the create path publishes
  seq-then-flags at dbi.c:623-626 (the ONE known seq-first window — readers
  robust per INV-C2/dbi-state-machine).
- **Errors**: `MDBX_NOTFOUND` (absent, no CREATE), `MDBX_INCOMPATIBLE`
  (flags), `MDBX_DANGLING_DBI`, `MDBX_DBS_FULL`, `MDBX_ENOMEM`;
  slowpath bind failures go through `gone` which CLOSES the env slot
  (`dbi_close_locked` → seq bump) while the fastpath returns without
  closing — both recoverable (handles re-import from MAIN), see §6 F-V4-3.

### dbi_update (dbi.c:265) — txn-end DBI settlement
- **Pre**: called exactly once per txn end (commit and abort), before/after
  cursor release per the txn pipeline.
- **Post**: commit ⇒ SLAIN dbis closed (env flags zeroed, name deferred,
  seq+1); abort ⇒ CREAT dbis rolled back the same way; the trailing
  all-invalid `n_dbi` shrink stops at CORE_DBS (the walk's condition is
  checked before the next decrement — the body's last run asserts the first
  USER slot, never FREE/MAIN); rename-undo discharged
  (abort: names restored under dbi_lock; nested commit: chain prepended to
  the parent — child undos apply first, reverse-chronological; basal commit:
  buffers to deferred-free with the lock acquired locally).
- **Note**: the publish branch at dbi.c:284-285 is dead (see §6 F-V4-2).

### dbi_import / dbi_check / dbi_gone / dbi_dig
- Verified per the wave-3 transition map (fact:verification:dbi-state-machine);
  statement-level additions: the SPARSE bitmap word-zeroing loop
  (dbi.c:46-47) zeroes only words fully above `n_dbi` (the partial word
  containing `dbi` is untouched — its low bits may be set); the non-sparse
  zero loop's `do … while (dbi >= ++i)` zeroes exactly `[n_dbi, dbi]` and
  lands `n_dbi = dbi+1`; `dbi_seq_next` never returns 0 (wrap guard);
  `dbi_dig`'s `default` assert case falls through to the ancestor walk in
  release.

### tbl_fetch (table.c:55)
- **Pre**: `mc` may be an UNINITIALIZED cursor — `cursor_init(mc, txn,
  MAIN_DBI)` initializes it (the dbi.c call sites pass raw stack couples by
  design); `name` non-null.
- **Post (success)**: `txn->dbs[dbi]` = the record copied from the
  (unaligned) node payload; flags mismatch gate
  `(wanna ^ db) & PERSISTENT && !ACCEDE && !(CREATE && root == P_INVALID)` →
  INCOMPATIBLE; release-checked `height >= CURSOR_STACK_SIZE →
  MDBX_CORRUPTED` (issue-#63 mirror, tree-ops.c root-collapse stack bound)
  and `mod_txnid > leaf-page txnid → MDBX_CORRUPTED`.

### tbl_create / tbl_purge / tbl_root_txnid / tbl_stat_summary
- tbl_create: the memset-zeroed `body` aliases `txn->dbs[slot]` — the field
  assignments (root=P_INVALID, mod_txnid, flags) happen BEFORE the
  `cursor_put_checklen` copy, so the persisted record is the SET one; the
  couple is registered in `txn->cursors[MAIN_DBI]` for the put and
  unregistered on every exit (:135/:138, INV-C4).
- tbl_purge: `tree->mod_txnid` intentionally NOT bumped inline (commented
  :181) — the commit path sets `mod_txnid = txnid` for DIRTY dbis
  (txn-basal.c:297/422-423; `DBI_DIRTY` set at :173); `root = P_INVALID`
  skips coherency_check's root-stamp comparison; all dbi cursors be_poor'd.
- tbl_root_txnid: for `MAIN_DBI` the dirty test is txn-level
  (`MDBX_TXN_DIRTY`), coarser than MAIN's own `DBI_DIRTY` — may return
  `front_txnid` when only a user dbi is dirty; every consumer uses the value
  as a *conservative hint* (frozen fast-path rejection / check_page_header
  bound), so overestimates are safe (INV-V4-3).
- tbl_stat_summary: scan gate `done < main.items + 1` ⟺ `opened < items`
  (done counts MAIN itself); the scan cursor is NOT armed with z_pagecheck
  in default builds (couple_init arms it only under CHECKS2/MDBX_VALIDATION)
  — node derefs trust A7, see §6 F-V4-5.

---

## 4. Proposed additions to `test-coverage.md` (gaps)

1. **BUNCHES_REMOVAL=0 compile target**: the default build never compiles
   tree-ops.c's `#else` (tree_drop walk) nor tree-cutoff.c's `#else`
   (tree_cutoff_range leaf loop). No CI job compiles
   `-DMDBX_ENABLE_BUNCHES_REMOVAL=0`, which is exactly how V-038 (the
   `cursor_pop` rename miss) survived. Gap: a CI smoke job (or amalgamate
   check) with `MDBX_ENABLE_BUNCHES_REMOVAL=0` — compile-only would have
   caught it; a runtime smoke of `mdbx_drop`/range-del through the legacy
   path would guard the semantics too.
2. **Dupsort range-delete boundary hives**: the BUNCHES removal path
   (cutoff_zikkurat/cutoff_storey) trims boundary hives via inner-driven
   deletions and finishes with one outer-driven deletion (the only one that
   may collapse a sub-page to a plain node). Gaps: tests where (a) the range
   ends mid-hive with the hive ≥ 3 rows, (b) the range's begin is mid-hive
   with exactly 2 rows left in it, (c) range end == hive's last row with
   `end_including`, (d) range spanning adjacent keys whose hives both get
   trimmed to the 2-row floor; assert `ms_entries` row-accounting and
   post-delivery GET_CURRENT/NEXT behavior on all affected cursors.
3. **Root collapse × stash interplay**: cursors positioned deep (height ≥ 4)
   while another cursor deletes a subtree forcing a multi-level collapse
   (root-collapse at top == 0 with stash ≥ 1). The over-long
   `top_and_stash` encoding (duplicate tail) is release-safe but no test
   walks a cursor through ≥ 2 consecutive collapses with stash ≥ 1 — a
   Debug-assert smoke (cursor_validate after each step) would pin it.
4. **node_move room pre-check exactness**: branch borrow with `cdst->ki == 0`
   where the position-0 key grows by an odd byte (dupfix `dupfix_size` odd
   feeding the branch separator) — the ≤1-byte shortfall must produce
   `MDBX_RESULT_TRUE` retry, never a stuck state. Micro-test: dupsort
   INTEGERDUP-style fixed odd sizes forcing separator growth at position 0.
5. **walk_pages depth contract**: no test exercises a visitor that returns an
   error at depth (chk's 42 cap is consumer-side). A unit probe with a
   deep-but-valid tree (height ~16) and a visitor erroring at depth N
   verifies the walk unwinds and frees the bitmap on the error path.
6. **tbl_stat_summary vs opened tables**: stats after opening a subset of
   named tables (some VALID, some STALE, some CREAT-in-txn) — the
   `done < items + 1` gate and the name-skip loop with duplicate names are
   untested (ms_entries must equal the full row count exactly once).

---

## 5. Assumptions (this tranche)

- **A7** (standing): default builds trust DB contents; `page_node`/entries
  derefs in tree-search, walk, and the tbl_stat_summary scan are
  VALIDATION-gated (z_pagecheck / MDBX_VALIDATION / MDBX_DISABLE_VALIDATION).
- **A-V4-1**: a committed or mid-txn stable tree never has an empty root
  leaf (tree_rebalance collapses to `P_INVALID`); `tree_search`'s
  `ki[0] = numkeys−1` on `numkeys == 0` (→ `(indx_t)−1`) is transient-only.
- **A-V4-2**: tree metadata counters (branch/leaf/large_pages, items) match
  content — `tree_drop`'s up-front `pnl_need` reservation and
  tree_cutoff_twig's item accounting are exact only under this; a mismatch
  degrades to a capacity-failure bailout (safe, txn error).
- **A-V4-3**: `clc`/`kvx` is per-dbi shared state; the DUPFIXED dispatch's
  lmin/lmax narrowing and the ordinal fast paths are sound because all keys
  carry `dupfix_size` (enforced at put/setup).

---

## 6. Findings

- **F-V4-1** (minor, DEBUG-only, LATENT): `cursor_dump_position`
  (tree-cutoff.c:8-39, `#ifndef NDEBUG`) accumulates snprintf *return
  values* into `tail`; once truncation occurs `tail ≥ end` and
  `end − tail` wraps to ~SIZE_MAX for every subsequent call → bounds
  violation. **Reachability: latent** — all 13 existing call sites pass
  `print_pageptr = false` (the `.%u` shape fits trivially even with the
  subcursor double pass); the overrun becomes live only if a future caller
  passes `print_pageptr = true` on a deep stack (top=31, 16-char `%p`,
  10-digit pgno → 32 chars/level × 32 × 2 passes ≈ 1090 > 1023).
  Machine-checked: `docs/verification/repro/v4-dump-position-overshoot.c`
  (buggy shape SIGABRTs under glibc fortify / runaway `tail = 1921` without
  fortify; the `-DFIXED` clamp — after **every** snprintf, pass-end clamping
  is too late — exits 0). Also: static buf = non-reentrant (debug-only).
  KB: bug:tree-cutoff:dump-position-latent.
- **F-V4-2** (dead code): dbi_update's publish branch
  (dbi.c:284-285, `commit && !SLAIN → publish flags`) is unreachable given
  the L270 filter `(commit ? DBI_SLAIN : DBI_CREAT) == 0 → continue` —
  commit processes only SLAIN dbis, which always take the close branch.
  Harmless (flags are published at open: dbi_open_locked:623-626,
  dbi_bind:388) but misleading; either drop the branch or fix the filter.
- **F-V4-3** (minor inconsistency): dbi_open's lock-free fastpath returns
  dbi_bind errors without closing the env slot, while the slowpath's `gone`
  label closes it (`dbi_close_locked` → seq bump). The same user error
  (e.g. `MDBX_INCOMPATIBLE` on re-open with different flags) therefore
  closes-or-not depending on a race-dependent path. Both outcomes are
  recoverable (other handles re-import from MAIN), no data loss — but the
  semantics should be pinned one way.
- **F-V4-4** (robustness/API note): `walk_pgno`'s C-stack recursion depth is
  bounded only by the visitor's return (all current consumers cap: chk 42,
  api-copy deep_limit, defrag walk_stack cap). The walk.h contract should
  document the visitor's depth responsibility; a future uncapped consumer
  allows C-stack exhaustion on a crafted deep chain of distinct pages
  (the revisit guard bounds total pages, not depth).
- **F-V4-5** (consistency note): `tbl_stat_summary`'s scan cursors are not
  armed with `z_pagecheck` in default builds (couple_init arms it only under
  CHECKS2/MDBX_VALIDATION) while `walk_tbl` arms unconditionally — the scan
  derefs nodes/entries unvalidated in release (bounded garbage stats under
  crafted DBs). Consistent with trust model A7, but the two read-only
  walkers have different defensive postures; aligning them (or documenting
  why) would remove the surprise.
- **V-038 scope re-verification** (KNOWN, confirmed): `MDBX_ENABLE_BUNCHES_REMOVAL`
  defaults to **1** (options.h:125) — the default build compiles
  `tree_cutoff_twig` and never sees the broken lines. With `=0` the build
  fails exactly at tree-ops.c:25/:87 (`cursor_pop` undefined since the
  `cursor_eject` rename); `-Dcursor_pop=cursor_eject` compiles tree-ops.c
  clean (`gcc -fsyntax-only`, exit 0), and `cursor_eject`'s semantics fit
  both sites (stash == 0 guaranteed; :87's popup runs at top ≥ 1; :25 at
  height == 1 ejects to top = −1 which skips the loop and retires pg[0]).
  Nothing else in the `#else` branches is broken; their runtime logic was
  verified statement-level (see §2 tree_drop legacy path).

## 7. Established lemmas (summary)

- LEM-V4-1 branchless lower_bound exactness + conservative `exact` flag.
- LEM-V4-2 branch separator convention (min-key for i ≥ 1, node 0
  don't-care) and the load-bearing position-0 refresh sites.
- LEM-V4-3 frozen-parent ⇒ frozen-children (parent-pointer update requires a
  dirty parent) + tbl_root_txnid's conservative-proxy property.
- LEM-V4-4 walk accounting exactness for all five page shapes.
- LEM-V4-5 cursor combo_state packing and the one-store poor reset.
- INV-V4-Z cutoff_zikkurat region discipline and exact partition of the
  other-cursor fixups.
