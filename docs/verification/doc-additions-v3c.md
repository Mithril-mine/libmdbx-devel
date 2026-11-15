# doc-additions-v3c — stream C, wave 3 (structure→consumer contract pairs)

Companion to `doc-additions-v2c.md` (pairs wave 2) and `wave2.md`. This wave
verifies four (primitive → consumer) pairs by enumerating **every** call site
of the primitive inside the consumer and checking pre/post-conditions at each
site. Findings: F-C1..F-C5 (see §5), refutations R-C1..R-C2 (see §6), new
invariants INV-C1..INV-C4 (§4). Repro: `repro/v3c-put-dangling-cursor.c`
(F-C5, ASAN stack-use-after-return confirmed).

---

## 1. Primitive contracts worth adding to `docs/engineering/module-interfaces.md`

### 1.1 `tree_rebalance(mc)` (tree-ops.c:593)

* **Pre:** `cursor_is_tracked(mc)` (registered in `txn->cursors[cursor_dbi(mc)]`);
  `mc->top >= 0`; `mc->pg[mc->top]` modifiable; `mc->stash == 0` for the
  cursor_del entry (cursor-mut.c:1081 asserts it) — the primitive itself does
  not check stash and its success post-condition *discards* any stash
  (see post).
* **Post (success):** the page at `mc->pg[mc->top]` satisfies the fill/room
  thresholds or the tree was restructured to make it so. On any internal root
  collapse the whole tracked stack of `mc` and of every related m3 cursor is
  shifted down by one with the loop bounded by `m3->top + m3->stash`, never by
  the on-disk `tree->height` (tree-ops.c:678-697; this is the issue-#63 hardening
  also mirrored in `meta_validate`/`tbl_fetch`/`table.c:110-115`).
  On return `mc->top_and_stash` is set from the recomputed level, i.e. the
  primitive may leave `stash == 0` with a raised `top`; callers must not rely
  on the entry top being preserved (cursor-mut.c:1099-1102 re-reads
  `mc->ki[mc->top]`/`mc->pg[mc->top]` after the call for exactly this reason).
* **Failure:** returns error only from `page_get`/`page_retire`/`MDBX_PROBLEM`
  (no-merge-possible); every such failure is fatal for the txn — consumers
  must set `MDBX_TXN_ERROR` (cursor-mut.c:1170-1172 does; a `MDBX_RESULT_TRUE`
  from `page_merge` is never propagated, it drives the neighbor-choice retry
  loop instead).
* **Other-cursor discipline:** adjustments iterate
  `txn->cursors[cursor_dbi(mc)]` with `m3 = (mc->flags & z_inner) ?
  &m2->subcur->cursor : m2` and `is_related(mc, m3)`; the adjustments happen
  BEFORE the parent-level recursion (page_merge's `tree_rebalance(cdst)` at
  tree-ops.c:571), so an m3 anchored page is never compared after it has been
  moved by a deeper-level adjustment of the same rebalance.

### 1.2 `tree_propagate_key(mc, key)` (tree-ops.c:903)

* **Pre:** `cursor_is_tracked(mc)`; `mc->ki[mc->top]` points at the branch node
  whose separator becomes `key`; page has room or the caller accepts a split.
* **Post (success):** separator updated in place (size-shift `memmove`) or,
  when `delta > page_room`, the node is deleted and re-added by
  `page_split(MDBX_SPLIT_REPLACE)`.
* **Failure:** can only fail through the split path, and `page_split`'s
  `bailout` sets `MDBX_TXN_ERROR` and `be_poor(mc)` (page-split.c, tail).
  Therefore a caller that propagates the error *without* setting
  `MDBX_TXN_ERROR` (cursor-mut.c:281-282, the `fix_parent` path of
  `cursor_put`) is nevertheless safe: the tree-state damage of the deleted
  separator node can only surface in a txn that `page_split` already marked
  errored. Documented here because it looks like a gap on a plain read of
  cursor_put (R-C1, §6).

### 1.3 `tree_drop(mc)` (tree-ops.c:13)

* **Pre (bunches-removal off):** `mc->tree->height > 0`; mc must be positioned
  by `tree_search(mc, nullptr, Z_FIRST)`; callers must tolerate a partial
  retirement on failure (some pages already in `wr.retired_pages`) and treat
  the txn as errored (both consumers do: cursor-mut.c:1034 → `fail:` label;
  table.c:166-170 sets `MDBX_TXN_ERROR` explicitly).
* **Pre (bunches-removal on, default):** `tree_cutoff_twig` recursion depth is
  explicitly bounded by `CURSOR_STACK_SIZE` (tree-cutoff.c:117-118) against
  cyclic branch pages of a crafted DB.
* **Retired-list discipline:** `pnl_need(retired_pages, branch+leaf+large)` is
  reserved up-front (tree-ops.c:27-30), so mid-walk `page_retire_ex` calls
  cannot fail for capacity. **Other cursors are NOT adjusted inside
  `tree_drop`** — the contract is that the *caller* invalidates them:
  `cursor_del` poors the related same-key subcursors (cursor-mut.c:1038-1041)
  and `tbl_purge` poors every cursor of the dbi (table.c:186-187). No other
  cursor may anchor into the dropped tree (per-key sub-trees are disjoint;
  the dbi-drop case is covered by the all-cursors be_poor).

### 1.4 `tree_deepen_edge(mc, Z_FIRST|Z_LAST)` (tree-search.c:112)

* **Pre:** mc not poor; `mc->pg[mc->top]` set; `mc->ki[mc->top]` = the child
  slot to follow.
* **Post:** mc stands on the first/last leaf of that child's subtree; every
  pushed page passed `page_get` (with the caller's `checking` mask).
* **Failure:** `be_poor(mc)` + error; all call sites
  (tree-cutoff.c:313, cursor-nav.c:365/402/449/496, api-copy.c:166,
  tree-search.c:24/73) propagate or bail out — none continue on error.

### 1.5 `walk_pages` / `walk_tbl` callback contract (walk.h:13, walk.c)

The visitor receives `(pgno, npages, userctx, deep, tbl, page_size, page_type,
page_txnid, page_err, nentries, payload, header, unused+align, parent_pgno)`.
Contract points consumers must honor (all three consumers — chk-walk, defrag —
do):

* `page_err != MDBX_SUCCESS` may arrive for a broken page: chk reports an
  issue and continues (chk-walk.c:179-180); defrag aborts (defrag.c:291).
* `npages == 0` identifies a sub-page record (not a file page);
  `page_type >= page_sub_leaf` marks sub-page types.
* **No page pointer crosses the callback** — only derived sizes/pgnos, so
  callback-side page lifetime is a non-issue by construction.
* Termination of the walk under `dont_check_revisit` relies on the visitor's
  return: chk's pagemap-based "already used/loop" detection returns
  `MDBX_RESULT_TRUE` for branches (prunes recursion, chk-walk.c:174-176) and
  chk additionally caps `deep > 42` (chk-walk.c:24-27); defrag caps
  `deep >= ARRAY_LENGTH(dfc->walk_stack)` (defrag.c:293).
* **Overflow-page chaining:** `walk_visit` marks only the base pgno of a large
  run in the visited bitmap (walk.c:224), but chk's pagemap marks the full
  span (chk-walk.c:156-171), so interior-page double references are still
  caught by the consumer; a page_get_large of a crafted interior page is
  rejected by the header checks (PAIR-N8) and reaches the callback as
  `page_err`.
* `walk_tbl` always arms the walk cursors with `z_pagecheck`
  (walk.c:347-348), so every visited page — including every N_DUP sub-page —
  passes full `page_check` (sub-page lower/upper within the node size,
  entries offsets within `[sp->upper, dsize-PAGEHDRSZ]`, keys within the
  sub-page end, page-get.c:296-331) **before** walk.c's size-computation and
  recursion loops call `page_numkeys(sp)`/`page_node(sp, ii)`. This is the
  load-bearing containment chain INV-C1 (§4): neither walk.c loop re-validates.

### 1.6 dbi state machine (dbi.c) — site-verified transitions

Full transition map as verified per site (DIRTY/STALE/FRESH/CREAT/VALID/
SLAIN/OLDEN/LINDO):

| site | transition |
|---|---|
| dbi_import:45-58 (sparse new) | all chain levels state=0, n_dbi lifted, sparse bit set → LINDO → VALID(+STALE for user dbis) or BAD_DBI |
| dbi_import:103-107 | dangling cursors → `(SLAIN)|OLDEN\|LINDO`, seq←env → MDBX_DANGLING_DBI |
| dbi_import:109-115 | used-then-closed/reopened → `(SLAIN)|OLDEN\|LINDO`, seq←env → MDBX_BAD_DBI |
| dbi_gone:20-34 | chain-wide `(SLAIN)|OLDEN\|LINDO`; early return when the level held FRESH\|CREAT (no env publication); basal level publishes `txn->dbi_seqs[dbi] = 0` (txn-local snapshot poison ⇒ `dbi_changed` true everywhere in the process) |
| dbi_bind:372-393 (re-create) | env POISON → seq+1 → tbl_setup → env VALID → seq+2 (published + cached); failure → `DBI_LINDO` + TXN_ERROR |
| dbi_open_locked:559-566 (new slot) | POISON → seq+1 → fetch/create → state `LINDO\|VALID[\|FRESH][\|CREAT\|DIRTY]` → publish flags+seq (623-626) |
| dbi_open_locked:634-642 (bailout) | txn state `&= LINDO\|OLDEN`, env flags=0, name deferred via dbi_close_locked |
| dbi_update:269-301 | commit: non-SLAIN → publish flags; SLAIN/abort-CREAT → name freed to defer chain, flags=0, seq+1 published |
| mdbx_drop:95-104 | committed-open → `SLAIN\|LINDO` (close postponed to commit); CREAT/FRESH → `LINDO\|OLDEN` + immediate close |
| mdbx_dbi_close:180-220 | DANGLING_DBI if a live write txn holds DIRTY/CREAT on the handle (basal + env->txn hazard re-check) |

Verified protocol properties:

* **INV-C2 (seq-last publication):** every publisher writes
  `dbs_flags`/`kvs.name` *before* the seq store — except
  dbi_open_locked:623-626, which stores the seq *before* flags. In the
  seq-first order `dbi_snap`'s double-load can accept a pair `(s+1, old-flags)`
  that never existed as a coherent env state; all readers re-validate after
  the full fence (dbi_open fastpath re-checks at :706-711 and :747-752;
  `dbi_changed` compares cached vs live seq), so the mismatch self-heals —
  transient wrong answers are confined to racy multi-thread opens. Documented
  as the P6 protocol's one seq-first site.
* **dbi_gone FRESH/CREAT early-return is currently unreachable** for its only
  callers (tbl_refresh failures): a level can carry DBI_STALE only when its
  import came from env (parent chain state 0 for that dbi at import time),
  hence no ancestor can hold CREAT/FRESH while the entry level is STALE.
  Keeping the branch is correct defensively; noted so future dbi_gone callers
  know the CREAT-clearing semantics (the created table would then survive the
  owner's abort because dbi_update's abort branch keys on DBI_CREAT).
* **Rename-undo chain mutation sites (V-028 follow-up):** (1)
  `dbi_rename_undo_attach` (dbi.c:246) — prepend, sets `MDBX_TXN_DIRTY` to
  guarantee discharge; (2) `dbi_rename_undo_discharge` (dbi.c:196) — called
  only from dbi_update:319-320, commit=false restores names under dbi_lock,
  commit+nested hands the chain to the parent (order preserved),
  commit+basal sends buffers to the deferred-free; (3) `txn->dbi_rename_undo`
  cleared at discharge entry. V-028's lock-failure gap additionally applies to
  the **nested-abort** site (F-C4, §5).
* **defer_free chain (MDBX_ENABLE_DBI_LOCKFREE):** items are stamped with
  monotime at append (dbi.c:158-163) and unlinked only when older than 1 s
  (:142-153), giving lock-free readers of `env->kvs[].name` a 1-second grace
  window closed by the seq re-check; `dbi_defer_release` releases
  `env->dbi_lock` on every path — callers must hold it (documented at
  dbi.c:238-240 and honored by dbi_open:789-794, mdbx_dbi_close:169-220,
  mdbx_drop:102-104, dbi_rename_locked_and_release:851-871).

---

## 2. New invariants for `docs/engineering/libmdbx-invariants.md`

* **INV-C1 — walk containment chain (z_pagecheck ⊥ walk size loops).**
  `walk_tbl` unconditionally arms its cursors with `z_pagecheck`
  (walk.c:347-348, both option branches). `page_get_inline` routes
  z_pagecheck fetches to `check_page_complete` → `page_check`, which fully
  validates each visited page including every embedded N_DUP sub-page:
  `sp->upper/lower` within the node's data size, entries offsets within
  `[sp->upper, dsize-PAGEHDRSZ]`, node ends and keys within
  `end_of_subpage` (page-get.c:284-378). Therefore the size-computation loop
  and the recursion loop of `walk_pgno` — which dereference
  `page_numkeys(sp)`/`page_node(sp, ii)` without further checks — cannot read
  outside the sub-page/outer page/map page. This is what makes walk.c's
  unvalidated second pass safe; removing `z_pagecheck` from walk_tbl would
  make the first crafted sub-page an OOB read.
* **INV-C2 — dbi seq-last publication with one seq-first exception.**
  Publishers store seq after flags (dbi_bind:389, dbi_update:293,
  dbi_close_locked:885, dbi_open_locked MAIN :472) — the one exception is the
  create-path publish at dbi_open_locked:623-626 (seq before flags). Readers
  (`dbi_snap` double-load, `dbi_open` fastpath double re-validation,
  `dbi_changed`) are robust to the seq-first ordering: a torn pair either
  fails a re-validation (slowpath) or self-heals via `dbi_changed` → import.
  See §1.6.
* **INV-C3 — temporary cursor couples are self-looped, not registered.**
  `couple_init` sets `couple->outer.next = &couple->outer` (cursor-setup.c:285);
  a stack couple therefore stays invisible to all `txn->cursors[]` walks
  unless its owner explicitly links it (the link must be undone on every
  exit). The explicit-registration sites and their pairing:
  mdbx_del (api-txn-data.c:243/246 — paired), mdbx_replace_ex
  (:380/453 via `bailout` — paired), tbl_create (table.c:135/138 — paired),
  dbi_rename_locked (:828/847 — paired), dbi_enumerate (:293/342 — paired),
  node_move/page_merge temporaries (tree-ops.c:196-206, 345-353, 378-386,
  766-769, 813-816, 848-851, 864-867 — all paired), cutoff_zikkurat's dozer
  (tree-cutoff.c:238-239/356 — paired). **mdbx_put violates the pairing at
  api-txn-data.c:301 (F-C5)** — the only unpaired exit of a registered stack
  couple found in the tree.
* **INV-C4 — cursor_may_not_be_stacked_aside_on_bad_size.**  The inverse
  statement of F-C5 as an invariant: *an API function that registers a stack
  `cursor_couple_t` in `txn->cursors[dbi]` must reach its unregister on every
  exit path, including validation failures discovered after registration.*
  Enforcement suggestion for review profiles: grep `txn->cursors\[.*\] = &`
  and require an equal number of reaches to the restore line.

---

## 3. test-coverage.md additions

* **T-C1 (blocks F-C5):** `mdbx_put(MDBX_CURRENT)` on a DUPSORT+DUPFIXED+
  INTEGERDUP table where the key holds ≥2 dups, `MDBX_ALLDUPS`/`MDBX_NOOVERWRITE`
  unset, and the new value length is outside `[clc->v.lmin, clc->v.lmax]`;
  follow in the same txn with any cursor op on the same dbi (e.g.
  `mdbx_del`). Expect BAD_VALSIZE and a *still-healthy* txn; repro
  `repro/v3c-put-dangling-cursor.c` (crashes/corrupts today, ASAN
  stack-use-after-return confirmed with clang). Add the "empty dupsort key
  then size-error" twin from the same code path (NOOVERWRITE set → EMULTIVAL
  without the bare return).
* **T-C2 (covers F-C1):** build matrix line `-DMDBX_ENABLE_BUNCHES_REMOVAL=0`
  — currently does not compile (tree-ops.c:25/87 call the removed
  `cursor_pop`). Add the config to the CI build matrix so the option cannot
  rot again.
* **T-C3 (walk containment):** a crafted-DB unit (tests/ut/issues pattern)
  with an N_DUP node whose sub-page header claims `lower` beyond the node
  data size, exercised through `mdbx_env_chk` (walk path) and, if exposed,
  `mdbx_env_copy(compacting)` — expect clean MDBX_CORRUPTED reporting, no
  sanitizer noise. Locks INV-C1.
* **T-C4 (dbi):** rename + abort under dbi_lock contention (a concurrent
  `mdbx_dbi_open` loop) covering the V-028/F-C4 discharge sites; plus the
  seq-first publish window at dbi_open_locked:623-626 exercised by a
  concurrent opener (expect at worst a transient BAD_DBI with retry success).

---

## 4. (see §2 — invariants are listed there to keep the numbering single)

## 5. Findings

| F-id | file:line | severity | description |
|---|---|---|---|
| F-C1 | src/tree-ops.c:25,87 | **medium** (build break) | `MDBX_ENABLE_BUNCHES_REMOVAL=0` does not compile: commit 86eb2a15 (cursor-stack `stash` rework) renamed `cursor_pop`→`cursor_eject` in cursor.h:402 and fixed tree-cutoff.c:197/api-copy.c:284, but missed the two `cursor_pop(mc)` calls in tree-ops.c's non-bunches `tree_drop` walk. Reproduced: `gcc -fsyntax-only -DMDBX_ENABLE_BUNCHES_REMOVAL=0 src/tree-ops.c` → `implicit declaration of function 'cursor_pop'`. The default (bunches-removal on) and the amalgamated build are unaffected. Fix: replace with `cursor_eject(mc)` (same single-level pop; the walk keeps `stash == 0` at those points, satisfying the eject precondition). |
| F-C2 | src/api-txn-data.c:301 (+282-283) | **high** | `mdbx_put`: the MDBX_CURRENT multi-value size-precheck returns `MDBX_BAD_VALSIZE` with the stack `cursor_couple_t cx` still registered in `txn->cursors[dbi]` (registered :282-283, unregister :311 bypassed by the bare `return`). Every later cursor op on that dbi walks the dead frame (m2/m3 loops of cursor_del/insert/node_move/page_merge, `page_touch_unmodifiable`'s tracked-stack touch), and `txn_done_cursors` walks it at txn end. Reachable through the documented API: DUPSORT+DUPFIXED+INTEGERDUP table, key with ≥2 distinct dups, `MDBX_CURRENT`, data length outside `[clc->v.lmin, clc->v.lmax]`, `MDBX_ALLDUPS`/`MDBX_NOOVERWRITE` unset. Confirmed by ASAN (clang, `detect_stack_use_after_return=1`): `stack-use-after-return ... READ of size 1 ... page_touch_unmodifiable page-ops.c:270 ← cursor_del cursor-mut.c:932 ← mdbx_del api-txn-data.c:245`, victim frame `mdbx_put ... 'cx' (line 265)`. Fix: `goto` the shared exit (as `mdbx_replace_ex` does) or drop the registration before the precheck. |
| F-C3 | src/cursor-mut.c:285-289 | low | `cursor_put`/`fix_parent`: the post-propagate `MDBX_CURSOR_FULL` guard `mc->top + mc->stash + shift >= CURSOR_STACK_SIZE` double-counts — `top + stash` is invariant under `cursor_enroot`/`cursor_undo_enroot`, so the expression equals `original_top + shift`, and the undo itself only needs `top + shift == original_top ≤ 30` slots that already exist. For trees of height ≥ 17 whose leaf sits on an all-nonzero ancestor-ki path, a *successful* overwrite (leaf bytes and separator already written) is reported as `MDBX_CURSOR_FULL` with the cursor poored. No corruption; a spurious failure return. The branch should either be removed or rewritten against the true bound. |
| F-C4 | src/txn-nested.c:501 + src/dbi.c:273-275 | low (V-028 family) | V-028 enumerated the nested-commit and basal-abort sites of the dbi_lock-failure early return in `dbi_update`; the **nested-abort** site (`nested_undo → dbi_update(nested, false)`) is additionally exposed: the rename-undo chain stays on the nested txn, which is then freed — the undo nodes and old-name buffers leak and the rename is *not* rolled back (kvs keeps the new name while MAIN is rolled back). Gated on fastmutex-acquire failure and on some dbi being DBI_CREAT at abort; same mutex-failure severity class as V-028. |
| F-C5 | (merged into F-C2) | — | duplicate id reserved; see F-C2. |

## 6. Refutations / downgrades (verified non-bugs)

* **R-C1 — `cursor_put` fix_parent missing `MDBX_TXN_ERROR` (cursor-mut.c:281-282):**
  looks like an unmarked fatal path, but `tree_propagate_key` can only fail
  via its `page_split` branch, and `page_split`'s `bailout` sets
  `MDBX_TXN_ERROR` and `be_poor(mc)` (page-split.c, tail) — the broken-tree
  state (deleted separator, orphaned child) is unreachable for a commit.
  Downgraded to a documented contract note (§1.2).
* **R-C2 — `estimate_range` V-035 interplay:** `mdbx_estimate_range`/
  `mdbx_estimate_distance` use `cursor_diff` (api-range-estimate.c:13-77),
  which operates in `ptrdiff_t` over 16-bit `ki` values — the negative
  size_t-wrapping encoding of V-035 belongs to `cursor_distance_at`
  (cursor-seek.c) and is **not consumed** by the estimate path. Mixed
  eof_hard encodings at most flip an off-by-one sign for inverted ranges,
  which the API documents as returned as-is (the `#if 0` block at
  api-range-estimate.c:342-363). The `ASSERT(*size_items <= tree->items)` at
  :340 holds on all unclamped return paths (leaf-level and root-ki
  differences are bounded by the divergence-page fan-out, and the
  `height<4`/`leaf_pages==0` fallbacks return the same bounded raw diff).

## 7. Pair verdicts

| pair | verdict | notes |
|---|---|---|
| tree-ops.c → cursor-mut.c + tree-cutoff.c | **INCONSISTENT** (build only) | F-C1 is a build break, not a logic error. All 11 runtime call sites verified consistent: cursor-mut.c:280/1034/1084, tree-cutoff.c:313 (+ tree_drop→tbl_purge table.c:166); enroot/undo pairing correct (R-C1); root-collapse m3 shift bounded by top+stash (PAIR-N6 chain); `save_top−delta` (V-020) confirmed at page_merge tail; the FRESH/CREAT-independent failure paths all set TXN_ERROR. |
| walk.c → chk-walk.c + api-copy.c (+defrag) | **CONSISTENT** | dont_check_keys_ordering: walk level armed via z_ignord (chk_tree hardcodes it, chk-walk.c:242); chk's own kv-order checks are governed by the separate `MDBX_CHK_IGNORE_ORDER` (chk-walk.c:514-518, 572-576) — two distinct mechanisms, both respected. Clamped-record containment at chk-walk.c:713-796 verified (clamp formulas bounded by `data->iov_len`, span pass skipped for clamped records). api-copy `compacting_walk`: own-stack discipline verified (deep_limit = first-leaf depth; `mc->top >= deep_limit` guard precedes every stack write; buf covers deep_limit+1 pages; dupfix leaves only reachable on inner walks where the node loop is skipped). Third consumer found and verified: defrag.c:280-315/919. |
| dbi.c → table.c + txn-basal.c + api-dbi.c | **CONSISTENT-A** | state machine verified at all mutation sites (§1.6); P6 seq protocol holds with the one seq-first site (INV-C2); dbi_gone FRESH/CREAT branch unreachable-for-current-callers documented; defer_free 1-second grace correct; V-028 extended by F-C4 (nested-abort variant). |
| tree-search.c → api-range-estimate.c + api-txn-data.c | **INCONSISTENT** | F-C2 (mdbx_put dangling registration, ASAN-confirmed). estimate arithmetic consistent (R-C2); no raw user-length memcpy remains in api-txn-data.c (only `memcpy_checklen` at :464; api-dbi.c:308 memcpy is fixed-size `sizeof(tree_t)`); mdbx_del/mdbx_replace_ex registration pairing clean. |
