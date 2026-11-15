# Verification wave 7, stream A top-up — txn pipeline, statement-level ledger

Scope: statement-level P1–P7 completion for the functions stream B (wave 2,
`doc-additions-v6-b.md`) did not ledger per-function, plus cross-checks of the
rest: `src/txn.c` (11 functions), `src/txn-ro.c` (start/reset/free/park/unpark),
`src/spill.c` (entry math, `spill_slowpath` FULL, keep/purge/remove/search),
`src/refund.c` (all three), `src/audit.c` (both), and the
`txn-basal.c`/`txn-nested.c` gaps (`txn_basal_checkpoint`, `txn_basal_rollback`,
`nested_merge`, `txn_nested_checkpoint`, `txn_nested_rollback`).

Verdict vocabulary follows `docs/verification/protocol.md` / LEDGER.md.
Reusable results cited: LEDGER L1, L5, L8; INV-T1..T10 (wave 2 B);
PAIR-N1/N2/N3, PAIR-F6/V-021; V-028..V-030; INV-S9-1/S9-3; LEM-V3-1 (INV-P1);
SORT-search; PNL-order. Line numbers refer to the audit tree
(`verification/master-audit`). No REFUTED verdicts in this tranche — no
reproducers required.

---

## 1. `src/txn.c`

### 1.1 `txn_latency_gcprof` (txn.c:6–38) — P2 field-by-field — **PROVED·A**

Copies `env->lck->pgops.gc_prof` into `latency->gc_prof`, field by field.
P2 (completeness/exactness, LP64): every assignment is a weak load followed by
either a plain copy or `osal_monotime_to_16dot16` (monotone ticks → 16.16
fixed-point seconds; lossy by design, never negative). Field coverage is total
over `latency->gc_prof`: work{counter,rtime_monotonic,xtime_cpu,rsteps,xpages,majflt},
self{same 6}, wloops, coalescences, wipes, flushes, kicks,
pnl_merge_work/self{time,calls,volume}, max_reader_lag, max_retained_pages —
matches the `gc_prof_t` member list in `layout-lck.h` exactly (enumerated).
No arithmetic beyond the conversion ⇒ no overflow.

P6: `pgops` lives in the shared lck mapping and is concurrently updated
(per-process lck sharing); a single pass can observe a torn mixture of
generations. The result is advisory statistics — the only consumer is the
user-provided `MDBX_commit_latency`. Assumption **A-v7-1**: advisory
statistics need not be internally consistent (wave-2 V-030 documents the
bounded-retry variant for `txn_abort`; here there is not even a retry).
`txn_commit`'s basal path additionally `memset`s the whole shared `gc_prof`
after the snapshot — deliberate reset-for-latency semantics, P2 trivial.
**PROVED·A** on A-v7-1.

### 1.2 `txn_gc_detent` (txn.c:40–47) — **PROVED·A**

```c
detent = mvcc_snapshot_oldest_rw(txn, true).oldest_txnid;
if (detent == env->gc.detent) return false;
env->gc.detent = detent; return true;
```
P1/P6: all 8 call sites (api-gc.c:173, api-txn.c:603, gc-put.c:1427,
defrag.c:125/158/160, api-env.c:1023, gc-get.c:1038/1390, env.c:167) run
inside a write transaction under the writer mutex; `env->gc.detent` has no
other writers ⇒ no race on the cache. `mvcc_snapshot_oldest_rw(may_lag=true)`
may return the cached previous oldest after 4 unstable passes
(mvcc-readers.c:128–139) and the distrust path (cached > steady, V-034
family) forces a rescan — both yield a value ≤ the true oldest.
Safety argument: the oldest live-reader txnid is monotonically non-decreasing
(new readers always receive newer txnids; readers leaving only raise the
minimum), so any stale detent is a *lower bound* of the true oldest, and GC
decisions keyed by detent over-retain. An over-high detent is the only unsafe
direction and is excluded: the scan result is `min(scan, steady_txnid)` and
steady ≥ every live reader snapshot. Also bounded by the 4-pass may_lag cap ⇒
P4 termination. **PROVED·A** (assumption: reader-table scans return a
lower bound of the true oldest; verified against mvcc-readers.c:100–150).

### 1.3 `txn_done_cursors` (txn.c:49–62) — P4/P5 per-dbi walk — **PROVED**

For each dbi (`TXN_FOREACH_DBI_ALL`, sparse-bitmap walk, dbi.h:38–52): null
the head first, then `do cursor = cursor_eot(cursor, txn) while (cursor)`.
- P4 termination: each `cursor_eot` returns the *saved* `cursor->next`
  (cursor-setup.c:232) before any mutation. Registration (`api-cursor.c:53–56`,
  `api-txn-data.c` save/restore sites) head-inserts into a chain whose bottom
  is the zeroed `nullptr` from `txn_alloc`/`txn_basal_create` ⇒ chains are
  null-terminated and acyclic at walk time; each node visited exactly once,
  O(n). The self-loops that `cursor_eot` installs in its no-shadow branch
  (`cursor->next = cursor`, cursor-setup.c:264) are written *after* `next` is
  saved and the head was already nulled — the walked chain never re-enters a
  visited node.
- P5 (no error paths): `txn_done_cursors` is void; `cursor_eot` failure is
  `ENSURE_OBJ` (fatal) only. Branch behavior: with `MDBX_TXN_ERROR` set (the
  abort path — set by `txn_abort` at txn.c:178 *before* this call) `cursor_eot`
  takes the restore branch (`*cursor = *shadow`, INV-T2); without it (commit
  paths txn-basal.c:321, txn-nested.c:629) it takes the merge branch — both
  release the shadow and leave `txn->cursors[i] == nullptr` with
  `txn_may_have_cursors` cleared.
**PROVED** (re-confirms INV-T2 from the walk side).

### 1.4 `txn_shadow_cursors` (txn.c:64–93) — P5 error-path state matrix — **PROVED**

Concrete trace, parent chain `c1→c2→c3→∅`, failure at `cursor_shadow(c3)`:

| step | action | c1.next | c2.next | c3.next | nested head | flag |
|---|---|---|---|---|---|---|
| 0 | initial | c2 | c3 | ∅ | ∅ | — |
| 1 | shadow(c1) ok, relink | ∅ | c3 | ∅ | c1 | may_have ✓ |
| 2 | shadow(c2) ok, relink | ∅ | c1 | ∅ | c2→c1 | may_have ✓ |
| 3 | shadow(c3) FAIL | ∅ | c1 | ∅ (untouched) | c2→c1 | may_have ✓ |

- The parent head `parent->cursors[dbi]` is *never* written (only read at
  entry) — it still points at c1 at every intermediate state.
- The failure exit sets `txn->dbi_state[dbi] = DBI_OLDEN|DBI_LINDO` and
  `txn->flags |= MDBX_TXN_ERROR`, returns `err`. The only caller chain
  (`nested_start` last statement → `txn_nested_create` txn-nested.c:486–491)
  always aborts on failure, so the broken parent chain window (c1 no longer
  leading to c2/c3) is closed by construction.
- Recovery: `nested_undo` → `txn_done_cursors(nested)` — gated on
  `txn_may_have_cursors`, which was set at the *first* successful shadow
  (per-success `txn->flags |= txn_may_have_cursors`, txn.c:90); hence failure
  at k=1 (nothing re-linked, flag unset, nothing to restore) and failure at
  k≥2 (flag set, k−1 shadows walked) are both exactly right. `cursor_eot`
  ERROR branch restores `*cursor = *shadow` (original `next` included) ⇒
  c1→c2→c3 reconstructed; the wait4eot skip (`continue`, cursor untouched)
  keeps its links original, so the chain closes through it too.
- `cursor_shadow` mallocs the backup before any cursor mutation
  (cursor-setup.c:211–219) — a malloc failure leaves the cursor untouched.
Statement matrix **PROVED** (consistent with INV-T2; the flag-set ordering is
the load-bearing detail wave 2 did not spell out).

### 1.5 `txn_commit` (txn.c:95–142) — 4-shape dispatch, P1 completeness — **PROVED**

Dispatch tree and exhaustiveness (P1):
1. `MDBX_TXN_ERROR` → `txn_abort` → returns `MDBX_RESULT_TRUE` on clean abort
   else the abort error. Precedes everything ⇒ an error-flagged txn of any
   shape is aborted, never committed (nested included).
2. fake-ro-nested (`MDBX_ENABLE_FAKE_NESTED_READONLY_TRANSACTIONS`, compile
   time) → `txn_nested_fakero_end` (userctx restore + flag clear).
3. `txn != env->txn`: (a) not `txn_ro_flat` → `EINVAL` "unknown", **no state
   mutation**; (b) ro but `parent/HAS_CHILD/basal_txn` → `PROBLEM` "strange",
   **no mutation**; (c) else `txn_ro_free` → SUCCESS. `env->txn` is only ever
   a writer (set by `basal_start_locked` / `nested_start`, cleared by their
   ends) ⇒ this shape captures exactly the ro txns.
4. `txn == env->basal_txn` → `txn_basal_commit`; on failure sets ERROR;
   `MDBX_RESULT_TRUE` mapped by `MDBX_NOSUCCESS_PURE_COMMIT` (default 0,
   options.h:338) to `MDBX_SUCCESS` (P2); latency snapshot + shared-gc reset;
   `txn_basal_end(txn, true)` runs *unconditionally* (also after commit
   failure), result `err == SUCCESS ? rc : err`.
5. else nested: linkage validation (`parent && parent->nested == txn &&
   parent->env == env`) else `PROBLEM` without mutation; `txn_nested_commit`
   (join, or abort+free on error), latency filled after the merge.

Any txn reaching step 5 survived 1–4 and `txn == env->txn` and
`txn != basal_txn` ⇒ it is the active nested. No shape falls through; both
"strange" exits are pre-mutation. The basal `owner == 0` pre-`txn_basal_end`
BAD_TXN guard in `txn_abort` (txn.c:187–189) re-verified: `owner` is
set/cleared by `lck_txn_lock/unlock` (wave 2, confirmed at lck-posix.c), so
`owner == 0` means "writer mutex not ours — nothing to unlock". Dispatch
**PROVED** (confirms wave-2 §2 `txn_commit` contract).

### 1.6 `txn_abort_after_resurrect` (txn.c:145–153) — P5 recursion — **PROVED**

Recursion on `txn->nested` first, then `txn_abort(txn)`. P4: the nested chain
is a finite tree (single parent per txn; built only by `nested_start`), depth
bounded by user nesting — terminates. P5: the recursive `txn_abort(nested)`
runs `txn_nested_abort` → `nested_free` which clears `parent->nested`
(txn-nested.c:546) before returning ⇒ the following
`tASSERT0(txn, !txn->nested)` holds, then the outer txn aborts with a clean
cursor/nested state. Signature gate: only handles with
`txn_signature` are aborted (freed/zeroed handles skipped). No error paths.
Caller: api-env.c:594 (env close resurrect path). **PROVED**.

### 1.7 `txn_abort` (txn.c:156–198) — latency double-read loop P4/P6; flags order — **PROVED·A**

- Latency: one unconditional `txn_latency_gcprof`, then for `txn != env->txn`
  (ro txns) a retry loop: `snap = *latency; fill(latency); memcmp` until two
  consecutive fills agree. P4: theoretically unbounded under continuously
  changing shared `pgops` — this is wave-2 **V-030** (info), re-confirmed at
  statement level; practically bounded. Additional pedantic note: the
  `memcmp` spans the whole `MDBX_commit_latency` including inter-field padding
  and fields never written here; padding is indeterminate-valued
  (C11 6.2.6.1/6.3.2.3 pedantry), practically stable on the same stack frame
  and only `gc_prof` bytes can differ. Recorded as nit **F-TXN-3**, no fix
  required (or compare only `gc_prof`).
- Flags order: `txn->flags |= MDBX_TXN_ERROR` **before** `txn_done_cursors`
  forces the `cursor_eot` restore branch — without it a nested txn's cursors
  would merge state into the parent on abort. Verified against
  cursor-setup.c:246/253 (the branch selector reads exactly this flag).
  Statement **PROVED** (the whole point of the ordering, as wave 2 §2 stated).
- Dispatch re-verified: basal+`owner==0` → BAD_TXN before `txn_basal_end`
  (1.5); nested → `txn_nested_abort`; else the tASSERT pins
  `(flags & (ro_flat|DIRTY|SPILLS|HAS_CHILD)) == ro_flat` then `txn_ro_free`.
**PROVED·A** (A-v7-1 + V-030 standing).

### 1.8 `latch_maindb_locked` (txn.c:205–235) — P1 case split + P6 seq publication — **PROVED**

State space of `env->dbs_flags[MAIN_DBI]` (uint8): `DB_VALID|flags`
(DB_VALID = 0x80, internals.h:643), `DB_POISON` (0x7f, all-ones of the
7-bit payload = exact bitwise complement of DB_VALID), or 0. Called under
`dbi_lock`, only when the caller's cheap test failed. Case table (P1):

| # | state | condition | action | post-state |
|---|---|---|---|---|
| a | DB_VALID & == txn's flags | only reachable via `!txn->dbi_seqs[MAIN]` | return loaded seq, no mutation | unchanged |
| b | DB_VALID & mismatch & `env->basal_txn && env->txn != txn` | concurrent in-process writer changed main flags | `INCOMPATIBLE`, no mutation | unchanged |
| c | DB_VALID & mismatch & else | our own writer lineage changed flags | seq_next → POISON → publish seq → `tbl_setup` → on success seq_next → `DB_VALID\|flags` → publish seq | re-latched or stays POISON |
| d | !DB_VALID (0 or POISON) | never latched / latched-interrupted | same as (c) tail | re-latched or stays POISON |

- Disjointness (P2): legal MAIN table flags are enumerated by
  `check_table_flags` (cogs.h:187–200): only DUPSORT/DUPFIXED/REVERSEKEY/
  INTEGERKEY/DEFAULTS combos (low byte ≤ 0x3E) ⇒ `DB_VALID|flags` always has
  bit 7 set, `DB_POISON` (0x7f) never equals any valid encoding, and the
  `(uint8_t)` cast in the store is exact for the legal set.
- Case (a) seq sanity: every `DB_VALID` publisher writes a nonzero seq under
  `dbi_lock` (`dbi_seq_next` returns ≥ 1, dbi.h:93–96; both publishers —
  this function and `dbi_open_locked` dbi.c:471–472) ⇒ "DB_VALID with env seq
  0" is unconstructible, so the early return never publishes seq 0 and the
  caller's `tASSERT(txn->dbi_seqs[MAIN_DBI])` (txn.c:280) holds.
- POISON residue after `tbl_setup` failure (case c/d): self-healing — the
  next `txn_setup_primal` fails the cheap test, takes the lock, hits case (d),
  retries. No other reader consumes `dbs_flags[MAIN]` without the seq check.
- P6 publication order (cases c/d): flags write **then**
  `atomic_store32(seq, mo_AcquireRelease)`, in that order in both paths —
  the seqlock pattern: any observer that acquires the new seq sees the new
  flags (program order before the release store); observers on the old seq
  re-import via `dbi_changed`. Identical discipline to `dbi_open_locked`
  (dbi.c:455–472), and the POISON-epoch double bump is benign (readers just
  re-import). **PROVED**.

### 1.9 `txn_setup_primal` (txn.c:237–370) — P2 geo arithmetic, two clamps, Coverity note — **PROVED**

- P2 txnid/front: `front_txnid = txnid + ((flags & (WRITEMAP|RDONLY)) == 0)` —
  pure malloc writers get `txnid+1`, WRITEMAP/ro get `txnid` — matches
  LEM-V3-1's stamp model exactly.
- P2 the three regimes vs `env->dxb_mmap.current` are pairwise exclusive:
  grow `required > current` (ro uses `used_bytes`, writer `size_bytes`), shrink
  `size_bytes < current`, neither. `required ≤ size` always (first_unallocated
  ≤ end_pgno) ⇒ grow excludes shrink; grow returns immediately (after the
  upper-bound check `upper ≤ MAX_PAGENO+1` and page-alignment round-trip
  `bytes2pgno(pgno2bytes(upper)) == upper`, else `UNABLE_EXTEND_MAPSIZE`),
  so the shrink block is reached only in the disjoint regime. Neither-regime
  returns `err = MDBX_SUCCESS` from the initialization at txn.c:296.
- Shrink clamp (P2): under remap_lock (+resize_mutex on Windows), re-read
  `filesize`, then `current = min(current, limit, filesize)` — a monotone
  lowering toward the on-disk size; the guard
  `eASSERT(filesize ≥ required_bytes)` rests on the committed-meta-implies-file
  invariant (wave-2 shrink-two-phase-arith: bottom ≥ largest ≥ fua) —
  **PROVED·A** on that standing lemma (A-v7-2).
- Coverity `LOCK_EVASION` pre-lock check (txn.c:309–314), formalized: the
  gate `size_bytes < dxb_mmap.current` is read without the lock, but the
  block it guards (i) only *lowers* `current` toward `filesize`, (ii) re-reads
  `filesize` under the lock and re-derives the clamp from it, and (iii) does
  not touch the mapping or any correctness-sensitive decision — the
  correctness-sensitive grow decision (`required > current` → `dxb_resize`)
  is taken before, under no dependency on the gate. Race matrix: a racing
  grow (raises current) ⇒ at worst our clamp lowers `current` again and the
  next transaction re-enters the grow path (extra resize, no incorrectness);
  a racing shrink ⇒ idempotent; a stale-true gate ⇒ no-op when
  `current ≤ filesize` already. A stale-false gate ⇒ a missed clamp, which
  leaves `current` larger — safe (all page offsets remain within limit).
  ⇒ the pre-lock check cannot create any inconsistent outcome.
  **PROVED**.
- Windows lock order: `resize_mutex` (leaf) acquired *before*
  `remap_lock(shared)`, released in reverse — same order as `dxb_resize`
  (comment txn.c:334–339), no cycle (readers never take resize_mutex) —
  consistent with the module-lock-matrix; **PROVED·A** (matrix cross-ref).
- INV-T7 cross-check: `txn_setup_primal` is called only from
  `basal_start_locked` (txn-basal.c:176) and `ro_start_continue`
  (txn-ro.c:230) — i.e. basal/ro only, never for a nested writer whose
  `dbi_seqs` alias the parent's (confirms INV-T7's "only unconditional
  dbi_seqs writer" claim).

### 1.10 `txn_check_badbits_parked` (txn.c:372–390) — P1 — **PROVED**

Precondition (asserted, and guaranteed by the only caller shape): `PARKED ∈
bad_bits` and `txn->flags & bad_bits != 0`. The equality test
`(flags & (bad_bits|AUTOUNPARK)) != (PARKED|AUTOUNPARK)` admits exactly one
state: flags ⊇ {PARKED, AUTOUNPARK} and *no other bad bit*. Complete case
table:

| flags (relevant bits) | result |
|---|---|
| PARKED only | BAD_TXN (explicit unpark required) |
| PARKED + ERROR / OUSTED / FINISHED / HAS_CHILD | BAD_TXN (broken parked txn terminates, must not be used) |
| PARKED + AUTOUNPARK (nothing else) | `mdbx_txn_unpark(txn, false)` |

The tASSERT enumerating `bad_bits == BLOCKED` or `BLOCKED − ERROR` is exact
for the reachable callers: enumerating every `check_txn` site with
`PARKED ∈ bad_bits` (cogs.h:449 is the only entry into this function) yields
exactly two — `mdbx_txn_park` (api-txn.c:151, `BLOCKED−ERROR`) and
`mdbx_env_info_ex` (api-env.c:1389, full `BLOCKED`); every other site
(api-txn.c:36/167/285/344/420/726/757) excludes PARKED and returns BAD_TXN at
the cogs.h gate before dispatch. Flag values: `BLOCKED = FINISHED|ERROR|
HAS_CHILD|PARKED` (mdbx.h:1665). **PROVED**.

Quirk (info, **F-TXN-2**): `mdbx_txn_park(txn, autounpark=true)` on a txn
already parked *with* AUTOUNPARK falls through to `mdbx_txn_unpark(false)`
and silently *unparks* it, returning SUCCESS — a deterministic
idempotence violation of "park", fail-safe direction, documented here.

### 1.11 `txn_alloc` (txn.c:392–427) — P2/P3 layout vs `txn_basal_create` (INV-T7 cross-check) — **PROVED**

- P2 arithmetic (exact): `size = base + [ro: bitmap_bytes + max_dbi·sizeof(seq)]
  + max_dbi·(dbs + cursors + dbi_state)`; wiring `[prefix][dbs][cursors]
  [seqs(ro)][sparse(ro)][state(end-anchored)]` — each pointer is
  `prev + count·elemsize`, and `dbi_state = txn + size − max_dbi·sizeof(state)`
  coincides with the additive chain (`dbs+cursors+seqs+sparse` bytes between),
  i.e. sum(formula segments) == sum(pointer hops) for both flag variants.
  `STATIC_ASSERT(sizeof(wr) > sizeof(ro))` makes the ro prefix strictly
  smaller; no overlap for any max_dbi ≥ CORE_DBS.
- P3: `memset(txn, 0, base)` zeroes exactly the visible prefix (whole
  `MDBX_txn`, incl. `wr` for writer-nested allocations) — hence
  `nested_start`'s `tASSERT(!nested->wr.repnl)` and `rkl_destroy`-on-zeroed
  safety (INV-T1 S6) hold by construction. The checking-build
  `cursors[FREE_DBI] = nullptr` pre-guard prevents a SIGSEGV inside a later
  assert.
- INV-T7 cross-check re-verified from this side: `txn_alloc` is called with
  `txn_ro_flat` (api-txn.c:266/383) or nested flags only (txn-nested.c:480);
  `txn_basal_create` only for `env->basal_txn` (api-env.c); the GC couple is
  strictly basal-relative (L5). Writer-nested allocations leave
  `dbi_seqs/dbi_sparse == nullptr` and `nested_start` shares the parent's
  pointers before any use (txn-nested.c:373–375). No flags/max_dbi
  combination misaligns anything today — V-029's "refuted as active bug,
  re-use hazard" verdict stands. **PROVED**.

---

## 2. `src/txn-ro.c`

### 2.1 `txn_ro_start` (txn-ro.c:237–288) — FULL statement-level — **PROVED·A**

Statement walk:
1. `flags = txn_ro_flat|FINISHED|(env & (WRITEMAP|NOSTICKYTHREADS))`.
2. `ro_slot_get` → `ro_slot_alloc` (bind) or `ro_slot_checkpid` (reuse).
   Errors: `MDBX_BAD_RSLOT` for a thread-key slot with foreign pid
   (post-fork residue) or non-legacy-multiopen mismatch; bind errors
   propagate from `mvcc_bind_slot` (INV-S9-1 covers bind publication).
   Failure here: the handle carries only the fresh flags — nothing to unwind.
3. `prepare_only` path: asserts the handle is fresh (`txnid/owner/n_dbi == 0`)
   and the bound slot is clean (`snapshot_pages_used == 0` — published 0 by
   `mvcc_bind_slot`, INV-S9-1; `txnid` poisoned ≥ threshold). Then **flags are
   reset to exactly `txn_ro_flat|FINISHED`** — dropping the env-derived bits;
   safe because every later start path (mdbx_txn_refresh api-txn.c:209,
   renew :228, clone :383, unpark-restart :183) re-derives flags at
   txn-ro.c:239 before use. Returns without seizing ⇒ the txn is a valid
   "prepared" handle with no MVCC publication (nothing to clean).
4. Full path `ro_start_continue`:
   a. `owner` = slot tid or thread-self; overlap gate vs
      `env->basal_txn->owner` → `MDBX_TXN_OVERLAPPING` (unless
      LEGACY_OVERLAP) — fires *before* any seizure, slot still poisoned.
   b. `ro_seize` (2.2) or its error.
   c. `txn_setup_primal`.
   Failure ladder: `flags |= MDBX_TXN_PARKED; txn_ro_reset(txn)` — the forced
   PARKED steers `ro_slot_clean` into its parked branch
   (txn-ro.c:124–131), which unconditionally un-publishes the slot:
   `safe64_reset(txnid,false)` → store `tid = txn->owner` (nonzero — set in
   (a)) → yield-loop until poisoned-and-ours → sanitize tail →
   `snapshot_pages_used = 0` → `safe64_reset(txnid, true)` → refresh flag.
   Exhaustive over the ladder {OVERLAPPING, seize-PROBLEM/MVCC_RETARDED/
   other, setup_primal errors}: every exit leaves the slot poisoned, the
   handle FINISHED, no cursor/remap residue (reset releases the Windows
   remap lock and n_dbi). **PROVED** (ladder closed).
5. Post-success ordering: `txn->ro.base = dxb_mmap.base`; Windows: conditional
   shared remap-lock acquisition (shrink-needed, or Wine+grow-needed; skipped
   under NOSTICKYTHREADS to avoid recursive SRW — INV-T8 interplay note
   applies) with `txn_remap_locked|txn_shrink_allowed|txn_unmap_or_move_allowed`
   flags, released in `txn_ro_reset`; `dxb_sanitize_tail`; ENSURE
   `txnid ≥ cached_oldest`; table-flag asserts. The
   `pgno2bytes(first_unallocated) ≤ dxb_mmap.current` eASSERT is guaranteed
   by `txn_setup_primal`'s grow path (1.9). **PROVED·A** on INV-T8's Windows
   geometry assumption (standing).

### 2.2 `ro_seize` (txn-ro.c:143–213) — snapshot install ordering — **PROVED**

Per-iteration order (the load-bearing sequence):
1. `safe64_reset(&slot->txnid, true)` — poison FIRST (single-writer plain
   store of UINT64_MAX); scanners now skip this slot, so no torn snapshot is
   ever visible.
2. Volatile loads of `head.geometry.first_unallocated` and
   `head.pages_retired`; acquire fence; **re-verify**
   `meta_txnid(head.ptr_v) == head.txnid` — a recycled/being-rewritten meta
   page invalidates the values read above ⇒ re-tap troika, retry.
3. Publish `snapshot_pages_used/retired` (relaxed — external observers are
   excluded by the poisoned txnid until step 4), then
   `safe64_write(&slot->txnid, head.txnid)` — the snapshot becomes visible
   atomically with the txnid.
4. `rdt_refresh_flag = true (AcquireRelease)`; `meta_should_retry` re-tap;
   `coherency_fetch_head` (RESULT_TRUE → retry; hard error → return);
   cached-oldest gate (`txn->txnid < snap_oldest` → retry, or
   MVCC_RETARDED in stuck-meta mode); final self-check
   `txn->txnid == slot txnid` before SUCCESS.
Loop bound 42 → `MDBX_PROBLEM` bailout (P4 bounded; the caller's failure
ladder (2.1.4) cleans up). MVCC justification: a reader slot with valid
txnid T makes writers retain everything ≥ T; pages published here are
exactly head=T's snapshot; the final self-check closes the race where an
external cleaner re-poisoned between step 3 and the oldest check.
`badbits recheck` at start level = the ENSURE at txn-ro.c:283–284 plus
dxb_sanitize_tail — statement-level covered above. **PROVED** (consistent
with INV-S9-1's publish-order model, now per-statement).

### 2.3 `txn_ro_reset` (txn-ro.c:290–314) — **PROVED**

Order: cursors done (if any) → `n_dbi = 0` (blocks DBI activity first) →
Windows remap-lock release + flag clear → `ro_slot_clean` → FINISHED +
`owner = 0` + `txnid = INVALID_TXNID`, unconditionally, before returning rc.
P5: `ro_slot_clean` errors (BAD_RSLOT from `ro_slot_checkpid` — fork case)
return rc while the finish-state is still applied — correct, the handle is
dead regardless; the foreign-pid slot is deliberately *not* touched (a forked
child must not poison the parent's slot; matches `ro_slot_release`'s pid
guard). The `!slot` path clears PARKED and succeeds (without-lck mode); the
`!env->lck` path detaches the slot without reset — reachable only during env
teardown (lck freed), recorded as note **F-TXN-4** (no leak in practice:
env_close unmaps the reader table wholesale). **PROVED**.

### 2.4 `txn_ro_free` (txn-ro.c:316–326) — **PROVED**

`slot ? (txn_ro_reset + ro_slot_release) : nothing`, then
`tASSERT(no cursors)`, `signature = 0`, `osal_free`. P5: `ro_slot_release`
asserts the slot txnid is already poisoned (post-reset precondition holds —
reset ran first), then `safe64_reset(txnid, false)` (idempotent poison) and
clears pid only for non-TXKEY slots (thread-key slots are thread-owned;
pid kept so the slot stays bound to the thread) and only when
`own_pid` matches (fork guard: child env->pid == 0 ⇒ skip, per
`rthc_afterfork`). P3: signature zeroed before free — a later
`txn_abort_after_resurrect` skips the dead handle by signature. **PROVED**.

### 2.5 `txn_ro_park` (txn-ro.c:328–365) — flag/tid transitions — **PROVED·A**

Gate: flags exactly `txn_ro_flat` (assert + runtime BAD_TXN) and slot tid <
OUSTED; no slot ⇒ EPERM (without-lck mode — parking is impossible in
principle, comment verified: exclusive mode has no writer, no ousting).
Slot-integrity probe: pid/txnid/tid snapshots vs `env->pid/txn->txnid/
txn->owner` → PROBLEM/BAD_RSLOT on mismatch (slot stolen or ousting raced
before park). Broken txn (`MDBX_TXN_ERROR`) → `txn_ro_reset` (parking a
broken txn finishes it; documented in txn_check_badbits_parked's comment).
Transition sequence (P6): store `tid = MDBX_TID_TXN_PARKED`
(UINT64_MAX, AcquireRelease) → `rdt_refresh_flag = true` → **then**
`txn->flags += PARKED [| AUTOUNPARK]`. While parked: slot txnid stays VALID
(this is the MVCC retention point); other processes' hsr/cleanup observe
tid=PARKED and decline/oust (wave-4 hsr ledger). The local flags update
happens after the shared publish — a concurrent same-txn API call from
another thread is out-of-contract (txn ops are owner-bound). P2: the `+=`
is an OR because the gate proves both bits absent. **PROVED·A** (out-of-api
concurrency excluded).

### 2.6 `txn_ro_unpark` (txn-ro.c:367–428) — safe64 CAS ladder, flag/tid transitions — **PROVED**

Gate: flags exactly `txn_ro_flat|PARKED` (no FINISHED/HAS_CHILD) — else
`MDBX_OUSTED` iff flags == `txn_ro_flat|OUSTED`, else BAD_TXN (the mask
`(FINISHED|OUSTED|ERROR|ro_flat) == (ro_flat|OUSTED)` pins the ousted shape
exactly). No slot ⇒ EPERM.

Main loop state matrix (per iteration, reads pid relaxed, tid/txnid via
safe64_read):

| observed | action | exit |
|---|---|---|
| pid ≠ env->pid | PROBLEM (fork hazard) | immediate |
| tid == OUSTED ∨ txnid poisoned | break | reset path → OUSTED |
| tid ≠ PARKED ∨ txnid ≠ txn->txnid | break (slot mutated externally) | reset → OUSTED |
| flags & ERROR | break | reset → BAD_TXN (`broken`) |
| `ro.base ≠ dxb_mmap.base` | break (mapping moved while parked; INV-T8's load-bearing check) | reset → OUSTED |
| all good | CAS tid PARKED→owner (64-bit) / high-store+CAS32 (32-bit) | below |

- CAS failure (`continue`): the only tid writers are hsr (OUSTED), cleanup
  (poison), and ourselves — a lost race implies one of those ran; the next
  iteration observes OUSTED/threshold/mismatch and breaks. Termination under
  honest adversaries: each successful CAS exits; endless PARKED↔X flipping by
  external activity is the V-036 family (info, standing). **PROVED·A**.
- 32-bit path (P6): store `tid.high = owner.high` (relaxed), CAS
  `tid.low PARKED→owner.low`; on failure restore `high = PARKED.high`
  (AcquireRelease) and continue — the slot returns to a consistent PARKED
  value. The transient mixed value `(owner.high, 0xFFFFFFFF)` is a
  valid-looking tid: it can only affect same-pid thread-identity comparisons
  (e.g. `basal_check_overlapped`), where a mismatch is fail-safe and an
  equality collision is a ~2⁻³² coincidence with a spurious OVERLAPPING
  outcome (safe direction). Note **F-TXN-5**, no action.
- Post-CAS re-verification (the load-bearing double read): re-read txnid and
  tid; mismatch → break → reset path. This closes the oust race
  (poison-txnid then tid=OUSTED, mvcc-readers order): we may CAS between the
  two external stores; the re-read then sees poisoned txnid ⇒ break ⇒
  `txn_ro_reset` — which enters the parked branch (flags still have PARKED)
  and un-publishes cleanly (reset-loop until poisoned-and-ours). No state
  leaves a valid txnid behind. **PROVED** (state machine closed).
- Success: `flags &= ~(PARKED|AUTOUNPARK)`, SUCCESS. MVCC soundness: the
  snapshot txnid never dropped below oldest while parked (slot valid), base
  equal ⇒ mapping valid ⇒ all held references valid; newer commits are
  invisible to the snapshot (normal snapshot semantics). INV-T8 covers the
  base-ABA question itself; this ledger adds the flag/tid transitions
  above. **PROVED**.

---

## 3. `src/spill.c`

### 3.1 `txn_spill` entry arithmetic (spill.h:56–76) — **PROVED**

`wanna_spill_entries = need − dirtyroom − loose` (0 if no dirtylist);
`wanna_spill_npages = need + pages_including_loose|writemap.dirty_npages −
loose − dp_limit`. P2: `intptr_t` domain; operands are dp-limit-scale
(≤ dp_limit ≤ MAX_PAGENO < 2²⁴ on 32-bit with min pagesize; ≪ 2³¹) ⇒ no
overflow; negative results mean "nothing to free" and are gated by
`< 1` before `spill_slowpath`. Caller gate ⇒ `spill_slowpath` runs only when
at least one wanna ≥ 1. **PROVED**.

### 3.2 `spill_gate` (spill.c:178–188) — **PROVED** (note)

Clamps `part` to `[spill_min, spill_max]`, `spill_min = ceil(total/denom1) ≥ 1`
(denom 0 → 1), `spill_max = total − floor(total/denom2) ≥ 0`. Clamp order
(max then min) can raise part above spill_max when `spill_min > spill_max`
(e.g. total=1, denom1=1: min=1 > max=0) — the eASSERT `part ≤ total` still
holds; semantics: "spill at least the floor share". Note: since
`spill_min ≥ 1` and total ≥ 1 at both call sites (see 3.3), the
`if (!need_spill) goto done` guard at spill.c:207 is **dead code** (harmless;
info **F-TXN-6**). **PROVED**.

### 3.3 `spill_slowpath` (spill.c:190–448) — FULL statement-level — **PROVED·A**

Sequential ledger (fail-path exits marked):

1. **Entry accounting** (195–204): early-out when
   `loose_count ≥ pages_including_loose|writemap.dirty_npages` (everything
   dirty is loose — nothing spillable). `dirty_entries = length − loose ≥ 1`
   afterwards (some non-loose page exists ⇒ length > loose);
   `dirty_npages` likewise. `need_spill = max(gate(entries), gate(npages)) ≥ 1`.
2. **WRITEMAP branch** (213–243): asserts `spilled.list == nullptr` (WRITEMAP
   never builds a spill list); `dxb_msync(KICK)`; failure → **bailout**
   (TXN_ERROR). AVOID_MSYNC sub-path: dirtylist reset to loose-only —
   `setlen(0)` (sorted=0), `dirtyroom = dp_limit − loose`, re-append exactly
   `loose_count` 1-page entries ⇒ `pages_including_loose == loose` and
   `dirtyroom + length == dp_limit` — exact (P2, both asserts present).
   Pure-WRITEMAP sub-path: `unsynced += dirty_npages; spilled_npages +=
   dirty_npages; dirty_npages = 0` — bookkeeping transfer, exact.
   **done** exit.
3. **Spill-list init** (249–265): fresh → `least_removed = INT_MAX`,
   `pnl_alloc(need_spill)`, ENOMEM → **bailout**; existing → `spill_purge`
   (compact odd tags first) + `pnl_reserve` (rc ignored *by design* — append
   grows on demand; ledger note, safe since every later appender checks).
4. **Sort + keep** (268–282): `txn_dpl_sort` (ascending); `spill_txn_keep`
   stamps cursor-referenced pages' LRU words to `dirtylru` (age → 0), see
   3.5. `unspillable + loose ≥ dl->length` → ERROR "all dirty pages
   unspillable" → **done** (no TXN_ERROR; the done-exit maps to
   SUCCESS/TXN_FULL per 3.9 — V-021's abort-only-safety applies to the
   TXN_FULL consumer). Debug-mode early SUCCESS only when room suffices.
5. **LRU quantization** (308–334): `age_max` scan (P4 O(length)); kept pages
   have age 0 and every non-kept page has age ≥ 1 (`txn_dpl_lru_turn` in
   keep advanced the epoch) ⇒ `age_max ≥ 1` here and `age ≤ age_max` for all
   i, hence `age·reciprocal = age·(255<<24)/(age_max+1) < 255<<24 <
   UINT32_MAX` — the tASSERT at 160 is arithmetically exact, and
   `prio = age·reciprocal >> 24 ≤ 254` (256 reserved for keep; fresh pages
   in long-tail LRUs legitimately yield prio 0 → `256−0 = 256 = keep`).
   Large-page bias: `factor = prio·log2n_powerof2(pop2(npages)+1) + 157`,
   clamped to `255−factor` or 0 — overflow-idiom exact
   (`npages|npages>>1|...` = next pow2 − 1); asserts exact. The radix census
   (322–334) counts only `prio < 256`; bucket sums == spillable totals (P2).
6. **Threshold selection** (338–360): walk buckets until entries AND npages
   needs met (`prio2spill` = last needed bucket), extend `prio2adjacent` into
   the fresher half while cumulative < half of spillable+need; break.
   `tASSERT prio2spill < prio2adjacent ≤ 256` — holds in all enumerated
   shapes (empty-radix-head start: first iteration sets prio2spill ≥ 1).
   `iov_init(amount_entries, amount_npages, …)` reserves the upper bound;
   ENOMEM → **bailout**.
7. **Compaction/spill loop** (372–397): outer `while (r < dl->length &&
   (spilled_entries < need_entries || spilled_npages < need_npages))` — r
   strictly advances per iteration (P4). Spill predicate: `prio ≤ prio2spill
   || (prio < prio2adjacent && pgno contiguous with the previous spilled
   run's endpgno (`last`))` — I/O-adjacency extension, per the design
   comment. Selected run: `e = w; last = endpgno(w);` then backward
   extension `while (--w && endpgno(w) == items[w+1].pgno &&
   prio(w) < prio2adjacent)` (rewinds w over the run start; forward loop
   below re-spans [w+1..e]); then for i in (w, e]:
   `++spilled_entries; spilled_npages += npages; spill_page(...)` — failure
   → **failed** with r/w mid-flight.
   **spill_page** (63–72): asserts !WRITEMAP; `iov_page` FIRST, then
   `spill_append_span`. If the append fails after queueing, the page is
   written to disk but never tagged spilled — an aborted txn leaves it as
   garbage beyond the frontier (INV-T5 model: unreachable, never
   re-referenced). rc propagates ⇒ **bailout**. **PROVED·A** on INV-T5.
8. **failed-path accounting** (403–411) — PAIR-F6/V-021 cross-check at
   statement level: the tail loop `while (r < dl->length) items[++w] =
   items[++r]` skips the *failed* entry (first copy moves items[r+1] over
   slot w+1) and compacts the rest preserving order; then
   `dl->sorted = dpl_setlen(dl, w)` (sorted == new length: full sort
   preserved), `dirtyroom += spilled_entries`,
   `pages_including_loose −= spilled_npages`. The failed entry is counted in
   `spilled_entries` *and* dropped — the identity
   `tASSERT(r − w == spilled_entries)` therefore holds on BOTH paths (on
   success by construction; on failure because the counted-but-dropped entry
   appears exactly once in each bookkeeping). Shadow pages of dropped
   entries are released only by the abort path ⇒ accounting self-consistent
   **iff all failure exits abort** — exactly V-021/PAIR-F6's finding,
   re-confirmed per-statement. Then `iov_write` if non-empty (queued pages
   are written even on the failure path — abort-mandatory anyway),
   failure → **bailout**; success: `unsynced_pages += spilled_npages`,
   `pnl_sort(spilled.list, 2·first_unallocated)` (entries are `pgno<<1 ≤
   2·(fua−1) < 2·fua` — sort limit exact), `flags |= MDBX_TXN_SPILLS`.
9. **done exit** (445–447): `return dirtyroom + loose >
   min(need, CURSOR_STACK_SIZE) ? SUCCESS : MDBX_TXN_FULL`. Statement of the
   actual contract (refines wave-2's phrasing): spill never promises to
   satisfy `need` in full — SUCCESS is reported when the remaining room
   exceeds the *cursor-stack-clamped* requirement; `MDBX_TXN_FULL` honestly
   reports that even post-spill room is insufficient to continue.
   P2: `min(need, CURSOR_STACK_SIZE)` bounds the demand by the cursor depth
   budget (16 + WORDBITS/4).
Loop-level P4: all loops strictly advance (r forward, w only rewound within
an iteration then restored by the tail loop); bucket loop bounded by 256.
**PROVED·A** (A-v7-3: all consumer exits of TXN_FULL/bailout abort the txn —
verified: `txn_dpl_fault`/dml callers propagate to abort; spill's own
contract requires it, per V-021).

### 3.4 `spill_remove` / `spill_purge` / `spill_search` / `spill_intersect` — **PROVED**

- `spill_remove(idx, npages)` (spill.c:13–37): tags `list[idx] |= 1` (odd =
  removed), shrinks size only when idx == last, `least_removed = min(·, idx)`.
  The `npages > 1` walk (descending list, MDBX_PNL_ASCENDING == 0 per
  LEDGER facts): expects `list[idx−1] == list[idx] + 2` (adjacent lower page,
  exact arithmetic for `pgno<<1` encoding), tags it, updates least_removed;
  non-contiguous → break. Every touched idx is folded into `least_removed`
  ⇒ the invariant checked by `spill_check_odd` ("no odd entries below
  least_removed") is maintained by construction.
- `spill_purge` (39–59): compacts `[least_removed..len]` dropping odd
  entries, `pnl_setsize(w−1)`. Edge case verified: `least_removed == len+1`
  (all removals were tail-shrinks) — loop body runs zero times,
  `w−1 = least_removed−1 = len` ⇒ size unchanged, no inflation; in general
  `len ≥ least_removed−1` holds because a remove at idx shrinks the size only
  when idx was the last. `least_removed = INT_MAX` after purge; the
  all-even assert branch covers the nothing-removed case. **PROVED**
  (this closes the "least_removed > len" question raised during the audit —
  no bug, see 3.4 note).
- `spill_search` (spill.h:24–38): exact `pgno<<1` probe; odd (tagged) entries
  never match the even key ⇒ search naturally ignores tombstones.
  `spill_intersect` (spill.h:40–66): lower-bound probe of
  `spilled_range_last = ((pgno+npages)<<1)−1` against the descending list,
  hit iff `pnl[n] ≥ pgno<<1`; by SORT-search this is the exact
  intersection test for the even entries; a tombstone at the boundary can
  only produce a *conservative true* (over-cautious spill skip in
  `spill_prio`), never a false negative. CHECKS1 cross-checks per-page.
  Cites SORT-search + L1. **PROVED**.

### 3.5 `spill_cursor_keep` / `spill_txn_keep` (spill.c:76–125) — **PROVED**

- `spill_cursor_keep`: walks cursor levels i = 0..top+stash (finite; subcur
  descent depth 1 with is_subpage guard). For each non-frozen, modifiable/tmp
  page: binary-search the dirtylist; on hit with `pgno == mp->pgno` and
  `age > 0` (not yet stamped — the "не считаем дважды" guard): stamp the
  `size_t` word *immediately before* the page body
  (`ptr_disp(ptr, −sizeof(size_t))` — the LRU slot allocated with every
  shadow page) with `dirtylru` ⇒ `txn_dpl_age == 0` (asserted). Frozen pages
  break (they are not in this txn's dirtylist). P4 bounded by cursor depth.
- `spill_txn_keep`: `txn_dpl_lru_turn` first (epoch advance so every
  pre-existing page gets age ≥ 1 and only explicitly kept pages are age 0),
  then all dbis with `DBI_DIRTY|DBI_VALID` and live root, walking per-dbi
  cursor chains, skipping `m0` (already walked by the caller). The
  `mc != m0` skip covers m0 living in any dbi's chain. **PROVED**.

---

## 4. `src/refund.c`

### 4.1 `refund_reclaimed` (refund.c:6–31) — **PROVED**

Precondition (asserted, guaranteed by the caller gate 4.3): descending
`repnl` with `MDBX_PNL_MOST == first_unallocated − 1`. The comma-operator
loop `while (--first_unallocated, ++i <= len && pnl[i] == first_unallocated − 1)`
pops the contiguous descending prefix: after exit, exactly `i−1` pages were
popped, `first_unallocated == F − (i−1)`, and `pnl_setsize(len −= i−1)`
matches (P2 exact). The compaction `for (move < len) pnl[1+move] =
pnl[i+move]` is a forward shift-down (dst < src ⇒ overlap-safe), reading at
most `pnl[len_orig]` (in bounds, PNL-cap). The trailing
`tASSERT(pnl_check_allocated(repnl, fua−1))` pins the post-state. P4: loop
bounded by len. ASCENDING branch is dead (LEDGER fact). **PROVED**.

### 4.2 `refund_loose` (refund.c:33–171) — FULL statement-level — **PROVED**

Entry asserts pin the mode matrix: dirtylist present ⇔ `!WRITEMAP ||
AVOID_MSYNC`; absent ⇔ WRITEMAP-pure. Branch selection:
`!dl || dl->length − dl->sorted > loose_count` → **collect branch**, else
**sorted-tail branch**.

- Collect branch: `suitable` buffer = on-stack
  `MDBX_CACHELINE_SIZE·8/sizeof(pgno_t)` = 64 entries (64-bit) or
  `pnl_alloc(loose_count)`; alloc failure ⇒ **silent skip** (return) —
  benign per INV-T4 (watermark unchanged ⇒ retried when re-armed). Capacity
  is exact: w counts distinct chain nodes (chain length == loose_count,
  structural invariant) and the buffer is ≥ loose_count in either variant
  (assert at 64 pins it). The window test
  `fua − loose_count ≤ pgno` is underflow-free because
  `tASSERT(fua ≥ MIN_PAGENO + loose_count)` precedes (57). Contiguity gate
  `most + 1 == fua`: only when the best loose page sits directly below the
  frontier; else nothing refundable → falls through to watermark refresh
  (no-op attempt, INV-T4).
  Refund core: `pnl_sort` (descending; limit `MAX_PAGENO+1` ≥ any entry —
  entries ≤ fua−1 ≤ MAX_PAGENO, exact); descending run-scan extends `most`
  while `suitable[i] == most−1`; `refunded = fua − most`; updates
  `fua/loose_count`; dirtylist accounting `dirtyroom += refunded;
  pages_including_loose −= refunded` with the budget assert; single-pass
  compaction of `[pgno < most]` keeping sorted-prefix order
  (`dl->sorted = w` then `dpl_setlen(dl, w)` — explicit sorted assignment,
  INV-T9 respected). `unlink_loose` removes exactly the unlinked pages
  (pgno ≥ new fua) from the loose chain with non-WRITEMAP shadow release —
  count consistency: kept loose pages are all < fua−loose ≤ most ⇒ exactly
  `refunded` pages unlinked, matching `loose_count −= refunded`.
- Sorted-tail branch: `txn_dpl_sort` (⇒ sorted == length), tail scan
  `items[n].pgno == fua−1 && ptr->flags == P_LOOSE` decrementing fua per
  entry. Sentinel safety: `items[0].pgno == 0 ≠ fua−1` (fua ≥
  MIN_PAGENO+loose ≥ 4) short-circuits before the ptr deref ⇒ no OOB even at
  n == 0. Loose entries in a dirtylist are always 1-page (loose pages enter
  dl only via page_dirty of re-dirtied pages and spill's 1-page re-appends;
  multi-page entries are removed by page_wash before going loose) ⇒ per-entry
  fua decrement is per-page exact. `dpl_setlen(dl, n)` keeps
  `sorted == old length` (INV-T9, load-bearing here) ⇒
  `refunded = sorted − length`; then `dl->sorted = length`, accounting,
  shared `unlink_loose`. A non-loose entry stops the run and correctly stays
  dirty above the new frontier.
- Tail (167–170): `tASSERT(txn_dpl_check)`, free the maybe-heap `suitable`,
  and **always** `loose_refund_wl = fua` — including the no-op paths; this
  is what makes a failed attempt "one no-op retry per frontier move"
  (INV-T4), and pairs with `txn_refund`'s memo check (4.3).
P4: all loops bounded by chain/list lengths. **PROVED**.

### 4.3 `txn_refund` (refund.c:173–201) — the repnl↔loose alternation — **PROVED**

- Watermark gate (P1): writers of `loose_refund_wl` are exactly
  `refund_loose` (= fua), `page-ops.c:607` (raise to `pgno+2` when a page
  near the frontier turns loose — re-arms the gate), `gc-put.c:359` /
  `basal_start_locked` / `nested_start` (reset 0). Gate
  `loose && wl > fua` ⇒ "a loose page appeared at/above the last-seen
  frontier" — the gate opens on frontier-lowering or new loose pages, closes
  on any refund attempt. Anti-churn by design.
- Alternation loop: while `repnl MOST == fua−1` → `refund_reclaimed`
  (pops ≥ 1, lowers fua); then if `!loose || wl ≤ fua` break; else
  `memo = fua; refund_loose(); if (memo == fua) break`.
  P4 termination: each iteration strictly shrinks `repnl` (reclaimed pops
  only) or strictly lowers `fua` (loose refund only if it moved the
  frontier — memo guard); `repnl` is never refilled inside the loop ⇒
  bounded by `min(repnl_len, loose_count)` alternations. The watermark
  re-arm works because `refund_reclaimed` only *decreases* `fua`, flipping
  `wl > fua` back to true after each pop — re-confirms wave-2's `txn_refund`
  contract statement-level.
- P2: returns `before == fua ? false : true`; `spill_purge` runs only when
  the frontier moved (piggyback compaction of tombstones). Direction check:
  every spill-list consumer is safe/conservative with stale tombstones
  (3.4: search misses them, intersect is over-conservative, size accounting
  over-reserves) ⇒ purge timing is an optimization, not a correctness
  dependency. **PROVED**.

---

## 5. `src/audit.c`

### 5.1 `audit_ex_locked` (audit.c:27–106) — statement-level — **PROVED·A**

- `pending = loose + pnl_size(repnl) + (pnl_size(retired) − retired_stored)`:
  size_t subtraction; `retired_stored ≤ pnl_size(retired)` is the call
  contract (INV-T10's two shapes: commit-time passes the full size, mid-GC a
  partial count; both verified at gc-put.c/txn-basal.c call sites). A
  violating call would wrap `pending` huge and fail the identity (fail-closed,
  PROBLEM) — **PROVED·A** on the call contract.
- GC walk (41–56): key-size check → **CORRUPTED** fail-closed before any
  allocation (only the stack cursor couple exists — no leak); `len` =
  first pgno_t of the record value; filter
  `dont_filter_gc || !gc_is_reclaimed` (rkl contain over reclaimed ∪
  comeback, gc.h:82). Walk-error handling: `rc != MDBX_NOTFOUND` → ERROR log
  + tASSERT, and **release builds proceed** with a partial `gc` sum. Not a
  memory-safety issue; fail-closed in practice — an undercount breaks the
  balance unless an *independent* term overcounts by the same amount, and no
  mechanism couples them. Recorded as assumption **A-v7-4** + info note
  **F-TXN-7** (document the swallow; alternative: early-return rc).
- Bitmap (58–78): `done_bitmap_size = ceil(n_dbi/8)`; stack buffer covers
  n_dbi ≤ 4096 (MDBX_WORDBITS words), larger → osal_malloc, ENOMEM → clean
  return (no leak: cursor couple is stack). memset before use;
  `audit_dbi` writes only dbi ∈ [1, n_dbi) — in-bounds by construction.
  Free path (90–91) is reached on every continuation path (no returns between
  allocation and free except the ENOMEM one before use).
- Counting exactness (P2): `ctx.used = NUM_METAS + dig(FREE) + dig(MAIN)`;
  `dbi_enumerate(audit_dbi, ctx, dbi_locked=true)` — audit_ex holds
  `dbi_lock` so the locked variant skips the internal lock (P6: env->kvs/
  dbs_flags reads are lock-protected); the callback resolves each MAIN_DBI
  record through `dbi_dig` (live state wins) and marks the done-bitmap; the
  follow-up loop (78–88) adds only *non-enumerated* dbis with a live tree
  (no double count) and WARNINGs on the un-enumerated non-diggable state —
  INV-T10's model statement-level confirmed: pages of any table reachable in
  this snapshot are still counted through its MAIN record ⇒ a real leak
  unbalances the identity.
- Identity (93–105): `pending + gc + used == first_unallocated` → SUCCESS;
  else ERROR (both lines, with the second being the decisive one) +
  **MDBX_PROBLEM** — commit fails (CHECKS2 gate at txn-basal.c:429–435).
  All terms are size_t page/entry counts ≤ total pages ⇒ no overflow in the
  addition (32-bit: each term ≤ 2³²/psize·… bounded by MAX_PAGENO, sum fits
  since it is compared against a pgno-derived bound). **PROVED·A**.

### 5.2 `audit_ex` (audit.c:108–119) — **PROVED**

`dbi_lock` acquire → on success: preserve `txn->flags`, clear
`MDBX_TXN_BLOCKED` (so cursor_init/dbi checks accept the mid-commit or
blocked txn — the point of the masking), run `audit_ex_locked`, restore
flags exactly, `ENSURE_OBJ` on release (mutex release cannot fail). Failure
to acquire ⇒ returned rc, no state touched. P6: callers all hold the writer
mutex; contention only against same-process reader-side `dbi_open` — brief,
correct. INV-T10 cross-check: both call shapes (commit-time
txn-basal.c:430, `retired_stored = pnl_size(retired)`, `dont_filter_gc =
true`; mid-GC gc-put.c) verified. **PROVED**.

---

## 6. `src/txn-basal.c` — checkpoint / rollback

### 6.1 `txn_basal_checkpoint` (txn-basal.c:505–523) — statement-level — **PROVED**

1. `preserved_flags = flags & txn_rw_begin_flags`;
   `flags |= weakening & (NOMETASYNC|NOSYNC|SYNC_DURABLE)` — OR-only
   weakening (SYNC_DURABLE's absence-encoding means the durable direction is
   a no-op; only lazy bits are ever added — matches the parameter name).
2. `txn_basal_commit`:
   - SUCCESS → `txn_basal_end(txn, false)` (writer mutex *kept*) → SUCCESS →
     `txn_basal_start(preserved | txn_rw_already_locked)` — restart on the
     same handle under the held mutex (new txnid via
     `safe64_txnid_next`; checkpoint consumes a txnid — documented
     semantics).
   - end failed → return rc; `txn_basal_end` already unlocked on its own
     failure (`if (likely(unlock) || unlikely(rc != SUCCESS))
     lck_txn_unlock`, txn-basal.c:272–275) — the comment at 516 is exact.
   - commit failed → `flags |= MDBX_TXN_ERROR` (as `txn_commit` does, so the
     end's `dbi_update(txn, false)` rolls back — V-028's lock-failure caveat
     applies unchanged) → `txn_basal_end(txn, true)` (unlock) →
     `err == SUCCESS ? rc : err`.
P1 closed: every path either restarts with the mutex held or fully unwinds
(mutex released, handle FINISHED). P5: the restart window holds the writer
mutex throughout ⇒ `env->txn` flickers null only while this thread is inside
checkpoint; reader-side hazard walkers (api-dbi.c:180–207) tolerate the null
(`hazard &&` guards) — P6 **PROVED**. P2: rc mappings exact. dbi_rename_undo
carry-over into the restarted txn on the V-028 path — already registered
(V-028/V-047), cross-referenced. **PROVED**.

### 6.2 `txn_basal_rollback` (txn-basal.c:525–533) — **PROVED**

`preserved_flags` → `txn_basal_end(txn, false)` (commit flag = `!ERROR`: a
pre-errored txn rolls back dbi changes — correct semantics) → SUCCESS →
`txn_basal_start(already_locked)` restart. end failure → rc returned with
mutex released (end's own unlock-on-failure) and handle FINISHED — caller
must not reuse; API layer (mdbx_txn_rollback) rejects reuse via signature/
FINISHED checks (waves 5). No loops. **PROVED**.

---

## 7. `src/txn-nested.c` — nested_merge / nested checkpoint / rollback

### 7.1 `nested_merge` (txn-nested.c:7–339) — statement-level P1–P5 — **PROVED·A**

Pre: `(nested->flags & WRITEMAP) == 0` (WRITEMAP incompatible at create,
txn_nested_create:461–464); called only from `nested_join` after
`parent->geo = nested->geo` (line 664 precedes 673) — so every use of
`parent->geo.first_unallocated` below is the *post-child-refund* frontier.
All lists sorted ascending by `txn_dpl_sort` on entry (src = child's, dst =
parent's).

**Phase 1 — drop parent-dirty pages ≥ frontier** (13–24): descending tail
scan; each dropped entry `page_shadow_release`d (heap shadows; malloc-mode
only). P2 trace: dst=[4,6,7,9,11] (len 5, sorted 5), fua=8 → drop 11, 9;
stop at 7 ⇒ n=3, `dirtyroom += sorted−n = 2`, `sorted = setlen(3)`; budget
assert holds. **PROVED**.

**Phase 2 — sift parent-dirty ∩ child-reclaimed** (29–30):
`txn_dpl_sift(parent, parent->wr.repnl, false)` — the repnl here is the
child's (handed over at join 636–638); removes parent-dirty entries whose
pgno the child already reclaimed, releasing shadows and restoring dirtyroom
(dpl.c:441–521 walk; accounting assert inside). **PROVED** (dpl.c covered by
G1/pairs).

**Phase 3 — retired → reclaimed** (33–87): iterate child-appended retired
entries `[parent_retired_len+1 .. size]`:
- dirty hit (`txn_dpl_exist`): `page_wash` (stamps INVALID/P_BAD, removes
  from dl, dirtyroom++, shadow release); `l = 1`; multi-page (overflow)
  partial-reuse resolution: the backward scan
  `while (w > parent_retired_len && retired[w−1] == pgno+l) { --w; ++l; }`
  matches previously *compacted* pieces of the same span (kept entries are
  written at `++w`), so `l` reaches at most npages; fragments consumed
  earlier by other branches are already in repnl/spill-handled — the
  comment's "все осколки учтены" verified: every retired entry is consumed
  exactly once (either kept at ++w or inserted with its l), so each freed
  page is reclaimed exactly once.
- spill hit: `spill_remove(parent, si, 1)` — tombstone (spill list entries
  are per-page, so npages ≡ 1 here).
- neither: keep (`retired[++w] = pgno`).
- `pnl_insert_span(&parent->wr.repnl, pgno, l)` — `ENSURE_OBJ` (fatal on
  failure): capacity argument: join's `pnl_need(repnl, retired_delta)`
  reserves for the maximum possible insertions (Σl ≤ retired_delta —
  reserve is an upper-bound requirement, so `Σl ≤ delta` suffices; INV-T3's
  `Σl == retired_delta` phrasing is sufficient-not-necessary, the capacity
  form is what the code guarantees). Duplicate-insertion risk (repnl ∩
  inserted spans) rests on the structural repnl/retired disjointness —
  **PROVED·A** (V-018 family standing). Then `pnl_setsize(retired, w)`.
  P4: r strictly advances; the w-scan is bounded by parent_retired_len.

**Phase 4 — parent spill list filters** (90–172): purge first (compact
tombstones); then:
- refund filter: front run of entries `≥ 2·fua` (⇔ pgno ≥ fua) dropped from
  the descending head with exact index arithmetic (ASCENDING branch dead);
  these are stale parent-spill entries for pages the child refunded —
  verified direction (parent-spilled page re-dirtied by child ⇒ unspilled ⇒
  possibly refunded ⇒ entry stale).
- reclaimed filter: descending double walk s×r, match ⇒ `spill_remove(s,1)`
  (tombstone), each iteration advances s or r (P4).
- child-dirty filter: for each child-dirty entry range
  [from, from+npages): skip when disjoint (`dirty_from > spilled` → next
  dirty; `dirty_to ≤ spilled` → next spill), else `spill_remove` the single
  spilled entry and advance s — multi-page dirty entries remove each
  overlapping spill entry one per iteration (P4 exact).
- final `spill_purge` compacts the tombstones created above.

**Phase 5 — child-spilled vs parent-dirty** (175–180):
`txn_dpl_sift(parent, nested->wr.spilled.list, true)` (spilled ⇒ compares
`pl[i]>>1`): drops parent-dirty entries that the child spilled — the disk
holds the child's written version which wins; parent's shadow released by
sift. Budget assert present. **PROVED**.

**Phase 6 — merge accounting + the merge itself** (184–309):
- Range-compare loop: dst beyond src ⇒ keep dst (--d, ++l); src below dst ⇒
  **stamp** `sp->txnid = parent->front_txnid`, clear P_SPILLED (unless
  P_LOOSE), keep src (--s, ++l); overlap ⇒ dst entry killed
  (`ptr = nullptr`, shadow release, --d, **no ++l**). `dst->sorted =
  l+d+s` = exact merged length (holes excluded — killed entries not counted);
  `tASSERT(detent ≥ l+d+s)` pins capacity (join's `txn_dpl_reserve` covers).
  The stamp is `parent->front_txnid` — see **F-TXN-1** (KB fact
  `fact:verification:iov-page-disk` says "nested-merged pages stamped
  parent.front+1"; the code stamps parent.front — the +1 stamp is what the
  *child* pages carry before the merge, from `page_dirty` with
  `nested->front = parent.front+1`). Post-merge all parent dirty pages are
  uniformly stamped parent.front ⇒ `is_shadowed` at parent's iov_page re-stamp
  (LEM-V3-1's loop) — consistent; the KB fact's parenthetical needs the
  correction.
- Trailing-src stamp loop (214–222) stamps the src tail dst never reached.
- Extend branch (`dst->sorted ≥ dst->length`), merge from the end with
  gap-squash. Concrete trace (no overlaps): dst=[A4,B6,C9] len 3,
  src=[X5,Y8] len 2 → phase-6 counts l=4,d=1,s=0 → sorted=5 ≥ 3 ⇒ extend,
  l=5,s=2,d=3:
  C9 > Y8 → items[5]=C (d=2,l=4); B6 ≤ Y8 → items[4]=Y (s=1,l=3);
  B6 > X5 → items[3]=B (d=1,l=2); A4 ≤ X5 → items[2]=X (s=0,l=1);
  exit (s=0) → `ASSERT(l == d)`: 1 == 1 ✓; verify items[1..1] non-null ✓.
  Result [A4,X5,B6,Y8,C9] ascending ✓ — l ≤ d squash (holes) never triggers
  here because the end-merge never falls behind; with holes (overlap-killed
  dst entries) the squash compacts them toward the begin and re-derives d
  (P4: each squash strictly increases the gap ⇒ no infinite loop; total
  iterations bounded by length).
- Shrink branch (`sorted < length`, many dst kills). Concrete trace:
  dst=[A4,B5,C6,D7] len 4, src=[X4,Y6] len 2 → counts l=2,d=0,s=1 →
  sorted=3 < 4 ⇒ shrink, l=s=d=1. Squash-to-end: non-null of [1..4] = {D}
  packs at top [4..4], w=3 ⇒ d=4. Merge forward: D7 vs X4 → X wins? D7 ≥ X4
  ⇒ dst first: items[1] = X (s=2,l=2); D7 vs Y6 → Y: items[2] = Y (s=3,l=3);
  loop exits (s > src length) → `ASSERT(sorted − l == length − d)`:
  3−3 == 4−4 ✓; tail copy `while (l ≤ sorted) items[l++] = items[d++]`:
  items[3] = items[4] = D, d=5, l=4 > 3 stop ⇒ dst = [X4,Y6,D7] ✓ ascending,
  length 3 = sorted ✓.
- Tail accounting (306–315): `dirtyroom −= sorted − length` (net additions),
  `dpl_setlen(dst, sorted)`, `dirtylru = nested's`,
  `pages_including_loose` recomputed by an O(len) walk (deliberate recount,
  comment — cheaper than branching in the loops). P2 exact; budget asserts.
- src disposal (318–319): `setlen(src, 0)` + `txn_dpl_free(nested)`.
- Child-spill adoption (321–332): parent has a list ⇒ `pnl_merge` (must not
  fail — join pre-reserved `pnl_need(parent.spilled, nested size)` after
  `spill_purge(nested)`, so sizes count only live entries; both inputs clean
  (all-even) ⇒ L1's garbage-on-flag path not triggered) + free child's;
  else wholesale adopt including `least_removed` (index space moves with the
  list ⇒ consistent). Finally `MDBX_TXN_SPILLS` set iff a non-empty parent
  list survives (336–338).
P1: the fast-complete path (nested_join:687–694) skips the merge entirely
under asserts pinning no-dirty/no-spill/geo-equal. P5: no non-fatal failure
paths (ENSURE only). P6: writer mutex held throughout. **PROVED·A** (on
V-018 structural disjointness).

### 7.2 `txn_nested_checkpoint` (txn-nested.c:718–737) — **PROVED**

Linkage validation → PROBLEM without mutation. `nested_join(ts)`; on success
rebuild the handle: flags = preserved | parent's SPILLS, loose cleared,
`nested_start(nested, parent)` — the restart-begin is INV-T1-identical
(geo/dbi_seqs/dbi_sparse snapshot at 373–376 precedes the first fallible
`pnl_alloc` at 380 ⇒ a failed restart aborts safely; `tASSERT(env->txn ==
nested)` pins the linkage). join-or-start failure ⇒ `txn_nested_abort`
(consumes the handle; parent state per INV-T1 table). rc = the original
failure (abort's rc ignored — nested_undo always returns SUCCESS; parent
breakage signalled via flags inside undo's resize-undo failure path).
**PROVED**.

### 7.3 `txn_nested_rollback` (txn-nested.c:757–799) — **PROVED**

Linkage → PROBLEM; fake-ro-dirty ⇒ BAD_TXN. `flags |= MDBX_TXN_ERROR`
(restore-cursors semantics); `nested_undo` (always SUCCESS — the final
`if (rc != SUCCESS) txn_nested_abort` at 796–797 is **dead defensive
branch**, info note). On undo success: release what the previous
`nested_start` allocated but keep the handle — rkl clear/shrink ×2,
`txn_dpl_free`, `pnl_free(repnl→nullptr)`, `pnl_free(spilled→nullptr)`,
loose cleared, flags restored, then `nested_start` re-begin.
Start failure path (789–794): manual `nested_undo(nested)` +
`nested_free(nested)` — verified safe instead of `txn_nested_abort`
(the comment's "does not pass the checks" verified concretely):
undo re-checks !nested/!HAS_CHILD ✓, skips dbi_update (flags restored,
no DIRTY) ✓, retired handback valid ✓, geo comparison equal-parent ⇒
no spurious resize (#448 discipline holds for the restart's snapshot) ✓;
then free: parent linkage (set before first fallible op) ✓, rkl destroy on
either zeroed or copied state ✓, dpl/repnl free of the restart's own
allocations ✓, spilled null ✓. No double-free (undo frees nothing). Handle
consumed, rc returned. **PROVED**.

---

## 8. Findings (tranche-local IDs; coordinator to renumber into V-###)

| id | where | essence | severity |
|---|---|---|---|
| F-TXN-1 | txn-nested.c:200/218 vs KB `fact:verification:iov-page-disk` | KB correction: nested_merge stamps merged child pages with `parent->front_txnid` (not `parent.front+1`); the +1 stamp is the pre-merge child stamp from page_dirty. Final post-merge model unchanged (all parent dirty pages uniformly parent.front ⇒ iov_page re-stamp). | info (KB) |
| F-TXN-2 | api-txn.c:149–165 + txn.c:372–390 | `mdbx_txn_park(txn, autounpark=true)` on a txn already parked *with* MDBX_TXN_AUTOUNPARK routes through txn_check_badbits_parked → mdbx_txn_unpark(false): silently unparks and returns SUCCESS. Deterministic, fail-safe, but violates park idempotence; document or reject. | info |
| F-TXN-3 | txn.c:164–172 | The latency stability loop `memcmp`s whole `MDBX_commit_latency` incl. never-written fields and inter-field padding (indeterminate values — pedantic UB, practically stable). Compare only `gc_prof`, or memcmp the written range. Companion to V-030. | info |
| F-TXN-4 | txn-ro.c:110–113 | `ro_slot_clean` with `!env->lck` detaches `txn->ro.slot` without un-publishing the slot (txnid possibly valid). Reachable only while the env is being torn down (lck freed); env_close unmaps the reader table wholesale, so no observable leak — worth a comment pinning the precondition. | info |
| F-TXN-5 | txn-ro.c:406–411 | 32-bit CAS path exposes a transient mixed tid `(owner.high, PARKED.low)` that reads as a valid tid; only thread-identity comparisons can observe it, outcome is at worst a spurious (safe) MDBX_TXN_OVERLAPPING with ~2⁻³² coincidence. No action; document. | info |
| F-TXN-6 | spill.c:207 | `if (!need_spill)` is dead: `spill_gate` returns ≥ 1 for every total ≥ 1, and both reachable totals are ≥ 1. Harmless; remove or downgrade to assert. | info |
| F-TXN-7 | audit.c:54–56, 76 | Release builds swallow GC-walk / dbi_enumerate errors (ERROR log + tASSERT only) and run the identity with partial sums. Fail-closed in practice (uncorrelated terms ⇒ balance breaks ⇒ PROBLEM), but an explicit early-return would remove the theoretical silent-pass coincidence. | info |
| F-TXN-8 | txn-nested.c:796–797 | Dead defensive branch: `nested_undo` returns MDBX_SUCCESS unconditionally, so `txn_nested_rollback`'s final `if (rc != MDBX_SUCCESS) txn_nested_abort(nested)` is unreachable. Keep as defense-in-depth with a comment, or drop. | info |

No REFUTED verdicts; no reproducers required. Standing findings re-confirmed
at statement level: V-021/PAIR-F6 (3.3.8), V-028/V-047 (6.1), V-029/INV-T7
(1.11), V-030 (1.7), INV-T1..T10 cross-checks throughout.

## 9. New lemmas / assumptions

- **LEM-TXN-1 (shadow-flag closure)**: in `txn_shadow_cursors`,
  `txn->flags |= txn_may_have_cursors` after each *successful* shadow makes
  the abort-path cursor walk exactly match the re-linked set for a failure at
  any position k (k=1: nothing re-linked, flag unset; k≥2: flag set).
  Load-bearing for INV-T2's reconstruction.
- **LEM-TXN-2 (spill failed-path identity)**: in `spill_slowpath`, the
  failed entry is simultaneously counted (`spilled_entries`) and dropped from
  the dirtylist by the tail compaction, so `r − w == spilled_entries` holds
  on success and failure alike, and `dirtyroom/pages_including_loose` deltas
  count each entry exactly once — the accounting is self-consistent iff
  every consumer exit aborts the txn (V-021).
- **LEM-TXN-3 (unpark post-CAS closure)**: the re-verification after the
  tid CAS (txnid/tid re-read) closes the hsr oust race
  (poison-txnid → tid=OUSTED ordered pair): any interleaving leaves the slot
  either properly unparked (txnid==txnid ∧ tid==owner) or routed to
  `txn_ro_reset`'s parked-release loop — no intermediate state survives with
  a valid txnid and a non-owned tid.
- **LEM-TXN-4 (detent lower-bound)**: `txn_gc_detent`'s value is a lower
  bound of the true oldest live-reader txnid at all times (monotone oldest +
  min-with-steady cap + may_lag returning the *previous* oldest), hence
  stale-detent GC decisions only over-retain.
- **A-v7-1**: advisory statistics (`pgops`) need not be internally consistent.
- **A-v7-2**: committed meta geometry ⊆ on-disk file (wave-2
  shrink-two-phase-arith).
- **A-v7-3**: every consumer exit of `spill_slowpath`'s TXN_FULL/bailout
  aborts the txn (V-021 consumer-side).
- **A-v7-4**: `audit_ex` callers pass `retired_stored ≤ pnl_size(retired)`
  (INV-T10 shapes).
- **A-v7-5**: structural repnl/retired (and repnl/loose) disjointness
  (V-018 family).

## 10. Coverage statement

Every statement of the target functions was walked; P1 conditions tabled,
P2 arithmetic traced (concrete array traces for nested_merge's two merge
shapes and refund's tail-pop), P3 verified against
`page_shadow_release/page_wash/txn_dpl_*` contracts, P4 termination argued
per loop, P5 exit maps enumerated, P6 under the writer-mutex/reader-mutex
disciplines plus the slot-publication order (INV-S9-1). Remaining PROVED·A
assumptions are enumerated in §9 and trace to standing wave-1/2 results.
