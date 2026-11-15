# Verification wave 2, stream B (adversarial, txn pipeline) — proposed doc additions

Scope: `src/txn.c`, `src/txn-basal.c`, `src/txn-nested.c`, `src/txn-ro.c`,
`src/spill.c`, `src/refund.c`, `src/audit.c` (plus their direct support:
`src/dpl.c`, `src/page-iov.c`, `src/dxb.c` sync/meta-txnid arithmetic,
`src/dxb-resize.c`, `src/cursor-setup.c` shadowing, `src/dbi.c` update/rename-undo).

All verdicts below follow the LEDGER protocol (`PROVED` / `PROVED·A` / `REFUTED`
/ `SUSPECT`). Nothing in this tranche produced a critical/high REFUTED; two low
findings and one refuted-risk (R7) are registered as V-023/V-024/V-025.

---

## 1. Proposed additions to `docs/engineering/libmdbx-invariants.md`

### INV-T1 — nested-begin linkage precedes the first fallible op

`nested_start()` performs its *irreversible* linkage — `parent->nested`,
`nested->parent`, `parent->flags |= MDBX_TXN_HAS_CHILD`, `env->txn = nested`,
`nested->wr.troika = parent->wr.troika` — **before** the first fallible
operation, and (since #448) snapshots `dbi_sparse`, `dbi_seqs` and `nested->geo`
also before it. Consequently every `txn_nested_abort()` of a partially
initialized nested transaction can rely on: (a) the parent back-link and
`MDBX_TXN_HAS_CHILD` being present, (b) `nested->geo` being equal to
`parent->geo` (so the resize-undo comparison in `nested_undo()` is a no-op and
never promotes a zeroed geometry), (c) `nested->wr.troika` matching the parent's
(asserted by `txn_nested_abort`).

Failure-site table (verified per site; "parent state" = what the parent
observes after abort):

| site | fail op | nested fields set | parent state after abort |
|---|---|---|---|
| S1/S2 | `pnl_alloc(repnl)` / `txn_nested_start_enomem` probe | none (repnl == nullptr) | untouched |
| S3 | `txn_dpl_alloc` | repnl allocated-empty | untouched |
| S4/S5 | `pnl_insert_span(parent->repnl)` / `txn_nested_loose_move` probe | repnl empty, dirtyroom copied | *partial loose absorption*: k first loose pages moved to `parent->wr.repnl`, `loose_count` decremented, their dirtylist entries washed (`dirtyroom` incremented); consistent — `loose_refund_wl` stale but benign (see INV-T4) |
| S6/S7 | `rkl_copy(reclaimed/ready4reuse)` | repnl = copy of parent's (never installed) | untouched; `rkl_copy` failure leaves dst `rkl_init`-ed (empty), so `rkl_destroy` in `nested_free` is safe |
| S8 | `txn_shadow_cursors` (`cursor_shadow` malloc) | repnl copy, retired_pages handoff done, dbi_state/dbs copied | retired_pages handoff restored by size-cast; partially-shadowed cursor list fully reconstructed (INV-T2); `dbi_state[MAIN_DBI] = DBI_OLDEN|DBI_LINDO` dies with the nested txn |

Destroy-safety used by the abort path on never-initialized fields (all
verified): `rkl_destroy`/`rkl_clear_and_shrink` accept a zeroed rkl (list ==
nullptr != inplace → `osal_free(nullptr)`); `txn_dpl_free`/`pnl_free` accept
nullptr; `txn_dpl_clear` iterates an empty list.

`txn->wr.gc.comeback` is never destroyed by `nested_free` — safe **because**
`gc_put_init`/`gc_update` are reachable only from `txn_basal_commit`
(txn-basal.c:400–402), so a nested transaction's comeback list is always empty
(asserted at nested_undo/nested_join/nested_free).

### INV-T2 — cursor shadow/re-link/restore reconstruction (load-bearing)

`txn_shadow_cursors(parent, dbi)` **never mutates `parent->cursors[dbi]`**.
It re-links each *live* cursor into the nested list (`cursor->next = nested
head`) while its `backup` (taken before re-linking) preserves the original
`next`. Cursors with `cur_signature_wait4eot` are skipped in place. On the
abort path `txn_done_cursors(nested)` → `cursor_eot()` (ERROR branch) restores
`*cursor = *shadow`, so every shadowed cursor regains its **original** next,
which reconstructs the complete parent chain *including cursors that were
never shadowed* (their links were never touched) and skipped wait4eot cursors.
Preconditions: `cursor_shadow()` mallocs before any mutation; a failed
`cursor_shadow` returns with the cursor untouched. The head of the parent list
therefore stays valid at every intermediate state, including a failure at the
k-th cursor of n.

### INV-T3 — nested_join post-«No further failures» is all-but-infallible

Everything after the comment at txn-nested.c:627 is pointer/list surgery with
pre-reserved space: `txn_done_cursors` (infallible), retired_pages/repnl/rkl
handoffs (pointer moves), DBI-state merge, geo/canary copy, `nested_merge()`
(in-place, reserves verified below), `txn_refund()` (failure of
`pnl_alloc` inside `refund_loose` degrades to a skipped refund, comment "not a
reason for transaction fail"), and the final `dbi_update(nested, true)`.

Reservations that make `nested_merge` infallible, verified arithmetically:
- retired insertions: each retired entry is consumed exactly once and each
  `pnl_insert_span` length `l` equals the number of retired entries consumed
  for that span, so `Σl == retired_delta == pnl_need(nested->repnl, ·)` reserve;
  the insertions target the buffer that becomes `parent->wr.repnl` (swap at
  txn-nested.c:636–638 precedes `nested_merge`), i.e. the reserved one;
- spill merge: `pnl_need(parent->spilled, nested->spilled)` covers
  `pnl_merge` (adopt instead of merge when the parent had no list);
- dirty merge: `txn_dpl_reserve(parent, src+dst lengths)` before the comment;
  failure returns `MDBX_ENOMEM` **before** the comment.

The only reachable failure after the comment is `dbi_lock` acquisition inside
the tail `dbi_update` (V-023 below) — its handling is state-consistent but
leaks; see the finding.

### INV-T4 — stale `loose_refund_wl` is benign

`refund_loose()` re-derives everything (suitability window, contiguity, `most`)
from the actual loose chain and `geo.first_unallocated`; the watermark only
gates *when* the refund is attempted. A stale watermark therefore causes at
worst one no-op refund attempt. This is what makes the partial-absorption
state of INV-T1 (S4/S5) safe without resetting `loose_refund_wl`.

### INV-T5 — basal-commit failure window contains no fallible op

Between a successful `txn_write()` and `dxb_sync_locked()` the only code is
the in-register/meta-image assembly (memcpy/poke of `head` fields,
`pages_retired` accumulation, `meta_set_txnid`, `dxbid` copy) — no allocation,
no I/O, no locks. Hence "half-written pages on disk" (data written, meta not)
can be entered only via `dxb_sync_locked()` itself, whose every failure sets
`ENV_FATAL_ERROR` (L8), and the abort path (`MDBX_TXN_ERROR` →
`txn_basal_end` → `dbi_update(false)` → unlock) is complete: written data pages
stay as unreachable garbage beyond the committed frontier (never referenced:
tree walks never touch pages ≥ snapshot `first_unallocated`; WRITEMAP msync
ranges are `[0, first_unallocated)`; non-WRITEMAP reads are gated by page
txnid stamps). Earlier commit failures (audit, `iov_init`, `gc_update`,
`update_tbl_roots`) all precede the first byte written by this commit.

### INV-T6 — the NOMETASYNC `meta_sync_txnid` magic reconciles

`dxb_sync_locked` stores `meta_sync_txnid = committed_txnid - dist(flags)`
(dxb.c:1161–1164); `txn_basal_commit` fires `need_flush_for_nometasync` iff
`meta_sync_txnid == head.txnid - dist(txn->flags)` (txn-basal.c:347–357).
Reconciliation results:

1. The commit-side `dist` omits the `MDBX_NOMETASYNC` test, but the enclosing
   `meta_sync_txnid != head.txnid` gate implies the *previous* call had
   NOMETASYNC (dist 0 would have stored `head.txnid` exactly). A spurious fire
   for a non-NOMETASYNC current commit is conservative-correct: its data-write
   fsync covers the pending meta page regardless (fsync is file-scoped, so it
   even covers meta written by *another process* through its own fd of the same
   file).
2. Mode mismatch across processes (FD-lazy vs WRITEMAP-lazy, dist
   `LAZY_FD ≠ LAZY_WRITEMAP`) yields a false *negative* → explicit
   `meta_sync(env, head)` — the documented cost of mixing the two methods.
3. After a crash the lock-file copy of `meta_sync_txnid` may be stale while a
   recovered meta page is steady → possible spurious fire → one unnecessary
   full-path commit; no correctness effect. Conversely a steady head always
   implies `meta_sync_txnid == head.txnid` was set by the commit that made it
   steady (in-place sign update is the only way to steady a head, and it
   updates the counter), so no false "nothing pending" is constructible.
4. The lazy meta is always written via `fd4meta == lazy_fd` for NOMETASYNC
   envs (env.c:459 assigns `fd4meta = dsync_fd` only when `MDBX_NOMETASYNC`
   is *absent*), so the "current commit's fsync covers it" premise holds.
5. The `skip update meta ... already steady` early return in
   `dxb_sync_locked` never updates `meta_sync_txnid`, but it requires
   `pending.txnid == head.txnid`, which the commit path cannot produce
   (`pending = head + 1`); it is reachable only from the env-sync path where
   the counter is already correct.

Verdict: ATTACKED → held; no false-positive/negative constructible that breaks
durability or MVCC. Known-hardening idea (optional): include the NOMETASYNC bit
in the commit-side dist for symmetry/documentation value.

### INV-T7 — basal txn flex layout is basal-relative by design (R7)

Field-by-field (LP64):

- `txn_basal_create`: `[MDBX_txn][cursor_couple_t GC][dbs][cursors][dbi_seqs]
  [dbi_sparse][dbi_state]`, `base = sizeof(MDBX_txn) + sizeof(cursor_couple_t)`;
- `txn_alloc(ro)`: `[MDBX_txn−wr+ro][dbs][cursors][dbi_seqs][dbi_sparse]
  [dbi_state]`;
- `txn_alloc(writer-nested)`: `[MDBX_txn][dbs][cursors][dbi_state]` — no
  `dbi_seqs`/`dbi_sparse` of its own; `nested_start` shares the parent's
  pointers (`nested->dbi_seqs = parent->dbi_seqs`, `dbi_sparse` likewise), and
  `txn_setup_primal` — the only unconditional `dbi_seqs` writer — runs for
  basal/ro only.

`txn_alloc()` is called only with `txn_ro_flat` (api-txn.c:266/382/675) or
nested flags (txn-nested.c:480); `txn_basal_create()` only for
`env->basal_txn` (api-env.c:526). The GC cursor couple is placed/used strictly
basal-relative (txn-basal.c:200, gc-get.c:951 `gc_cursor_init`). Therefore no
flags/max_dbi combination misaligns the pointers today. The drift becomes a bug
only if (a) a basal txn is ever built by `txn_alloc()` with writer flags (no
couple → GC cursor would overlay `dbs[0..]`), or (b) code starts using
`ptr_disp(txn, sizeof(MDBX_txn))` for arbitrary writer txns. Registered as
V-024 (refuted risk, re-use hazard documented). Cheap hardening: a comment or
`tASSERT` tying `gc_cursor_init` to `env->basal_txn`, and a `STATIC_ASSERT`
that `txn_alloc` is never instantiated with basal semantics.

### INV-T8 — unpark base revalidation versus mapping move (P6)

- POSIX: an unmap/remap that could move the base takes `lck_rdt_lock` and is
  **vetoed** if any local reader slot holds a valid txnid (parked slots keep a
  valid txnid) — so while a parked txn's slot is valid the base cannot move;
  `txn_ro_unpark`'s base comparison is therefore belt-and-braces, and whenever
  it *does* fire, the txnid check has already classified the txn as OUSTED.
- Windows: a remap needs the exclusive remap-lock (try-acquire, deferral on
  failure per #154); readers that acquired it shared at start (geo-conditional
  `txn_remap_locked`) defer any remap for their lifetime; readers without it
  are suspended through the remap. The remap **targets the same base by
  design** (`osal-mmap.c`: reserve old base with `MEM_RESERVE`, free it, then
  `NtMapViewOfSection`), so a same-base re-creation is the normal outcome: the
  unpark base check passes, and correctness rests on (i) page-txnid gating of
  content and (ii) the slot txnid/tid re-validation after the CAS. A
  *moved*-base remap under a parked reader is caught by the base comparison —
  this check is load-bearing exactly there (a thread suspended mid-unpark and
  resumed after the move would otherwise CAS its slot back and return SUCCESS
  with dangling pointers).
- Interplay note (pre-existing debt, #424): the base-check scheme assumes
  active readers either hold the remap-lock shared or are suspended through
  the remap; the suspend path does not reset reader slots, so it relies on the
  same-base outcome for address validity. Recorded as an assumption on the
  Windows geometry path, not re-verified in this tranche.

### INV-T9 — `dpl_setlen` does not clamp `sorted`

`dpl_setlen()` sets only `length` (and the P_INVALID stub); `sorted` is
adjusted by callers explicitly. This is load-bearing in `refund_loose()`'s
sorted branch (`dpl_setlen(dl, n)` keeps `sorted == old length`, making
`dl->sorted - dl->length` the refunded count) and in `txn_write()`/spill
accounting (`dl->sorted = dpl_setlen(dl, w)`). Any future "cleanup" that makes
`dpl_setlen` clamp `sorted` silently breaks the refund accounting.

### INV-T10 — audit identity and its model

`audit_ex` proves the page-ledger balance
`first_unallocated == pending(loose + repnl + retired-not-stored) + Σ GC
records (filtered by gc_is_reclaimed unless dont_filter_gc) + NUM_METAS +
Σ table stats (enumerated from MAIN_DBI + dug handles)`.
It is a *balance* check over maintained statistics, not a page-by-page
reachability walk: it trusts `ms_*_pages` counters, detects double-allocation
across tables (over-count) and any page missing from all ledgers (under-count),
and its cost is O(#GC-records + #tables), not O(pages). An un-enumerated,
non-diggable handle (`dbi_state != 0`, `dbi_dig == nullptr`, STALE-only) hits a
WARNING-only branch, but its pages cannot silently vanish: any page of any
table reachable in this snapshot is counted through its MAIN_DBI record by
`dbi_enumerate`, so a real leak still unbalances the identity. Verified for
both call shapes: commit-time (`retired_stored = pnl_size(retired)`,
`dont_filter_gc = true`, `loose_count == 0`) and mid-GC (gc-put.c:1471/1502
filtering + partial `retired_stored`; gc-put.c:1539 `+ pnl_size(repnl)`).

---

## 2. Proposed txn-primitive contracts for `docs/engineering/module-interfaces.md`

### `txn_nested_create` / `nested_start`
- Contract: on any failure the nested handle is consumed by
  `txn_nested_abort()`; the parent is left in one of the INV-T1 table states,
  all of which satisfy `txn_dpl_check(parent)`; `txn_shadow_cursors` failure
  marks `nested->dbi_state[MAIN_DBI] = DBI_OLDEN|DBI_LINDO` (dies with the
  handle).
- Requires: `gc_put` unreachable in nested txns (keeps `comeback` empty);
  `cursor_shadow` malloc-before-mutate; INV-T2.

### `nested_join`
- Contract: after the reservation block succeeds, the merge cannot fail
  (INV-T3) *except* the final `dbi_update`; a failure there still leaves the
  parent fully merged (the error is advisory), the handle is freed, no
  double-undo is possible (`nested->flags = MDBX_TXN_FINISHED` clears
  `txn_may_have_cursors` and `MDBX_TXN_DIRTY`, making `nested_undo` a no-op).
- Requires: retired_delta reserve arithmetic (INV-T3), structural repnl/loose
  disjointness (V-018), spill `least_removed`/LSB discipline.

### `txn_basal_commit`
- Contract: no meta page is touched before `dxb_sync_locked` (INV-T5); every
  failure up to and including sync leaves a cleanly abortable txn (data pages
  on disk are unreachable garbage); `ENV_FATAL_ERROR` is set on sync failure
  (L8) and on pre-sync `meta_sync` failure.
- Requires: INV-T6 for the `need_flush_for_nometasync` arithmetic; `dpl_setlen`
  semantics (INV-T9) for the `txn_write` accounting
  `dirtyroom += r − 1 − w` (entries dropped = queued-and-released + failed
  never-queued), and for the WRITEMAP early return nothing is released because
  `iov_complete`'s walk is skipped in WRITEMAP mode and dirty pages live in the
  mapping (cleanup happens via abort/discard).

### `txn_spill` / `spill_slowpath`
- Contract: all failure exits set `MDBX_TXN_ERROR` and are abort-mandatory
  (V-021, confirmed: the entry compaction and `spilled_npages` accounting are
  self-consistent only under abort); the `done:` exit reports
  `MDBX_TXN_FULL` honestly when even a full spill cannot satisfy `need`.
- Requires: `spill_gate` clamps to `[spill_min, spill_max] ⊆ [0, total]`;
  LSB (`pgno<<1|deleted`) bookkeeping never persists across abort (the list is
  freed whole in `txn_basal_end`/`nested_free`); re-dirtying a spilled page
  removes it from the list via `page-ops.c` spill_search/spill_remove paths.

### `txn_refund`
- Contract: `first_unallocated` never shrinks below any page that could still
  be referenced by this or a concurrent snapshot (repnl/loose pages are
  txn-local, uncommitted); WRITEMAP: pages above the shrunk frontier are never
  in any msync range and remain unreachable garbage; the repnl↔loose
  alternation loop terminates (each iteration either shrinks the frontier or
  breaks) and re-attempts loose refunds after every repnl tail-pop (the
  watermark comparison flips correctly because `refund_reclaimed` only
  *decreases* `first_unallocated`).
- Requires: repnl/loose disjointness; `dpl_setlen` non-clamping (INV-T9);
  on-stack `suitable` capacity guard `loose_count ≤ pnl_bytes2size(onstack)`.

### `txn_commit` (dispatch, four shapes)
- ERROR-flagged → `txn_abort` (restore-cursors branch forced by
  `flags |= MDBX_TXN_ERROR` before `txn_done_cursors` — without it a nested
  txn's cursors would *merge* their state into the parent on abort; the
  restore branch is the whole point);
- fake-ro-nested → `txn_nested_fakero_end` (userctx restore, flag clear);
- `txn != env->txn` ro → validate, `txn_ro_free`; "unknown"/"strange" returns
  EINVAL/PROBLEM without state mutation;
- basal → commit + unconditional `txn_basal_end(unlock=true)` (end() runs even
  on commit failure; `dbi_update` commit-flag is `!MDBX_TXN_ERROR`);
- nested → `txn_nested_commit` (join; abort+free on error).
- `txn_abort` for a basal txn with `owner == 0` returns `MDBX_BAD_TXN` *before*
  `txn_basal_end` — correct, since `owner` is set/cleared by
  `lck_txn_lock/unlock` (lck-posix.c:1086/1097), so `owner == 0` means "our
  writer mutex is not held" and there is nothing to unlock.

### `txn_setup_primal` (Windows resize dance)
- The pre-lock `size_bytes < dxb_mmap.current` check is Coverity-annotated
  LOCK_EVASION; the claim was verified: a concurrent `dxb_mmap.current`
  change by another thread cannot invalidate the subsequent logic — the block
  only *lowers* `current` towards the on-disk filesize, and any racing grow
  re-establishes `current ≥ required_bytes` before the next transaction uses
  it; the correctness-sensitive path (`required_bytes > current` →
  `dxb_resize`) is taken before and does not depend on the pre-lock check.
- Windows: the block mutates `filesize/current`, hence the
  `resize_mutex → remap_lock(shared)` ordering (leaf-lock comment), matching
  `dxb_resize`'s own ordering; lock order is globally
  `resize_mutex → remap_lock` with no cycle (readers never take resize_mutex).

---

## 3. Findings (this tranche)

| id | where | essence | severity |
|---|---|---|---|
| V-023 | dbi.c:273 + txn-nested.c:701, txn-basal.c:271 | `dbi_update()` returns early on `dbi_lock` acquisition failure *before* `dbi_rename_undo_discharge`. Nested: `nested_join`'s tail `dbi_update(nested, true)` failure leaks the nested txn's `dbi_rename_undo` chain (nodes + old-name buffers) in `nested_free` (`osal_free(nested)`). Basal abort: `dbi_update(txn, false)` failure leaves the undo chain on the *reused* basal txn; `basal_start_locked` does not clear it, so the next `dbi_update` discharges a dead transaction's rename-undo under the wrong commit flag (an aborted rename becomes permanent in kvs / a created-and-renamed slot leaks). Gated on `osal_fastmutex_acquire` failure (mutex error — unreachable on a healthy process), and the txn-level outcome is still state-consistent (unlock discipline in `txn_basal_end` holds; nested merge is complete). Fix shape: discharge-or-retain explicitly (e.g. retry the lock, or clear `dbi_rename_undo` in `basal_start_locked`/`nested_start`, or make the discharge lockless-atomic). | low |
| V-024 | txn.c:392 vs txn-basal.c:66 (R7) | Layout-drift *risk* REFUTED for all current flag/dbi combinations (INV-T7): `txn_alloc` never builds a basal txn; GC couple strictly basal-relative; nested writers share parent's `dbi_seqs`/`dbi_sparse`. Residual re-use hazard documented; hardening = assertion/comment pinning the invariant. | info |
| V-025 | txn.c:164–173 | `txn_abort`'s latency snapshot loop for `txn != env->txn` retries `txn_latency_gcprof` while the shared `pgops.gc_prof` changes under it — theoretically unbounded under sustained external GC activity (per-process lck sharing); practically bounded (GC bursts are not continuous). Info: document the eventual-consistency assumption or bound the retries. | info |

Regression anchors respected: #448 (verified as INV-T1(b) — the geo/dbi_seqs
snapshot is before the first fallible op and `nested_undo`'s comparison is
exact-equality against the parent's untouched geo), #449 (iov mid-batch
failure propagation verified in the `txn_write`/`iov_page`/`iov_write` walk —
the retried-add path releases via `iov_complete` and propagates `ctx->err`),
#452/#450/#454/#444 not re-touched (outside tranche).

## 4. Test-coverage gaps implied by the attacks

1. **Per-site fault injection of `nested_start`** (MPROBE
   `txn_nested_start_enomem`, `txn_nested_loose_move`) × {parent with loose
   pages, parent with spilled pages, parent with live cursors on MAIN_DBI
   (incl. one wait4eot cursor), parent with retired pages}: assert
   `txn_dpl_check(parent)`, cursor-chain integrity, `audit_ex(parent,0,false)
   == 0` after each failed begin. Currently only the geo/EPROM (#448) case has
   coverage.
2. **`txn_shadow_cursors` failure at cursor k of n** — needs a fault site in
   `cursor_shadow` (or an allocator failure rule) plus a multi-cursor MAIN_DBI
   setup; asserts full chain reconstruction (INV-T2). No coverage today.
3. **`need_flush_for_nometasync` mixed-mode matrix**: {FD-lazy, WRITEMAP-lazy}
   × {FD, WRITEMAP, AVOID_MSYNC} current commit × {same, other} process —
   assert exactly one of {skip explicit sync, explicit meta_sync} per INV-T6
   and no spurious ENV_FATAL. Currently uncovered.
4. **Mid-GC audit calls** (gc-put.c:1471/1502/1539) with crafted
   reclaimed/comeback sets — covered only as tASSERT in checking builds; a
   release-mode test asserting the identity for BIGFOOT multi-chunk stores is
   missing.
5. **Refund alternation stress**: sequences forcing repnl/loose interleaving
   at the frontier (`repnl {f-1}`, loose `{f-2}`, then reverse), overflow-page
   repnl spans, and `dpl_setlen` accounting (`dirtyroom + dl.length ==
   dp_limit` after every step). Partially covered by stresstest shadow-tuning;
   the *boundary* cases are not.
6. **Windows same-base remap + parked reader** (INV-T8): parked txn across a
   shrink that lands at the same base — assert unpark returns SUCCESS and
   subsequent reads pass page-txnid gating; and a moved-base case — assert
   MDBX_OUSTED. Windows CI only.
7. **dbi_update lock-failure path** (V-023): needs a mutex-fault probe to be
   reachable; at minimum an ASAN/leak-check run with an injected
   `osal_fastmutex_acquire` failure at nested commit and at basal abort.

## 5. Reproducers

- `repro/v6b-txnid-dist-magic.c` — standalone simulation of the
  `meta_sync_txnid` reconciliation (INV-T6): enumerates commit sequences
  (modes × dist) and asserts no false "pending" classification.
- `repro/v6b-layout-drift-check.c` — standalone layout arithmetic for
  `txn_basal_create` vs `txn_alloc(ro|writer)` (INV-T7/V-024): computes every
  pointer offset for parameter combinations and asserts non-overlap and
  mutual-consistency where the same field must coincide.
- `repro/v6b-nested-begin-fault-recipe.c` — the MPROBE-arming recipe (probe
  build) plus the field-state assertions for the INV-T1 table; compiles
  against the amalgamated probe build, otherwise degrades to documentation.
