# doc-additions-v4c — stream C, wave 4 (API surface: structure→consumer pairs)

Companion to `doc-additions-v3c.md` (wave 3) and `wave3.md` §INV-CC1..CC4.
This wave verifies the API-primitive layer: the INV-CC4 registration-idiom
sweep (pair 1), env lifecycle (pair 2), txn-ro park/unpark/clone + env_info
(pair 3), gc_info/reader-enumeration/cache-status machine (pair 4). Findings:
F-C4 is the only new code-level finding (hardening note); two refutations
R-C3/R-C4 document hypotheses that the wave killed (one statically, one
dynamically with `repro/v4c-close-lck-munmap.c`). New invariants INV-CC5,
INV-E1, INV-T9, INV-CACHE1.

Method note: every verdict below enumerates the consumer's call sites and
checks pre/post at each; "SAFE-A" marks a site whose safety rests on a
stated assumption that is argued but not machine-checked.

---

## 1. Contracts worth adding to `docs/engineering/module-interfaces.md`

### 1.1 `txn->cursors[]` registration idiom — the INV-CC4 mechanical sweep (pair 1)

INV-CC3/CC4 (wave 3): a `cursor_couple_t` is self-looped (`couple_init`,
cursor-setup.c:285) and invisible to every `txn->cursors[]` walk until its
owner explicitly links it; every explicit link must be undone on every exit.
The wave-3 fact (`fact:verification:temp-cursor-self`) listed 16 sites; the
wave-4 sweep re-grepped the whole tree (`grep -n 'cursors\[' src/*.c src/*.h
src/osal/*.h src/mdbx++/*.h++`) and completed the table — 7 sites were not in
the wave-3 list and are classified here for the first time (marked **NEW**).
The full table (registration file:line | restore | early-return paths between
| classification):

| # | site (reg → restore) | function | early-return paths between | class |
|---|---|---|---|---|
| 1 | api-txn-data.c:243/244 → :246 | mdbx_del | none (adjacent, single `cursor_del`) | SAFE (wave 3) |
| 2 | api-txn-data.c:282/283 → :311 | mdbx_put | `return MDBX_BAD_VALSIZE` :294-301 precheck bypasses restore | **V-037-LIKE = V-037 KNOWN** (bug:api-txn-data:put-dangling-cursor; reference only — not re-opened) |
| 3 | api-txn-data.c:380/381 → :453 | mdbx_replace_ex | all `goto bailout`; restore on ladder | SAFE (wave 3) |
| 4 | api-dbi.c:88/89 → :91 | mdbx_drop (MAIN-record del path) | none (adjacent, single `cursor_del`); all prior returns precede registration | SAFE (**NEW** vs wave-3 list) |
| 5 | api-dbi.c:293/294 → :342 | dbi_enumerate | all exits reach :342 | SAFE (wave 3) |
| 6 | api-misc.c:95/96 → :98 | mdbx_dbi_sequence (setter, MAIN touch) | none (adjacent, single `tree_search`); `return LOG_IFERR(rc)` after restore | SAFE (**NEW**) |
| 7 | api-gc.c:51/52 → :97 | mdbx_gc_info | loop exits only via `break` (gc_check_keylen, glr.err, iter_func rc≠FALSE, outer_next); restore unconditional after loop | SAFE-A (**NEW**) — assumes `iter_func` does not `longjmp` (same assumption class as every user callback) |
| 8 | api-cursor.c:52/53 | cursor_rebind | persistent registration of a real user cursor — pairing belongs to unbind/EOT, not this site | SAFE (persistent) |
| 9 | api-cursor.c:132, :207 | mdbx_cursor_close/unbind | unlink via `**prev` walk | SAFE |
| 10 | api-cursor.c:251-253 | mdbx_txn_release_all_cursors_ex | nulls head then walks | SAFE |
| 11 | api-cursor.c:753/754 → :917 (get_axe) | mdbx_cursor_bunch_delete | every switch-case exits via `break` (no `return` after `get_axe`); restore guarded only by `if (axe)` | SAFE-A (**NEW**) — the restore `head = axe->next` is unconditional; correctness requires axe to still be the head, which holds because the only intra-call registration (tree-cutoff dozer, #17) is itself paired |
| 12 | api-cursor.c:957-958 & 966-967 → :982-984 | mdbx_cursor_delete_range | `outer_first`/`outer_last` failures return **before** registration in each block; at most one block runs (`!begin && !end` rejected); between reg and restore only `tree_cutoff_range` | SAFE-A (**NEW**) — restore is conditional on `head == &couple.outer`; condition holds because #17 is paired and nothing else re-registers during the call |
| 13 | cursor-setup.c:124/125 → :127 | touch_dbi (MAIN record touch) | none (adjacent, single `tree_search`) | SAFE (**NEW**) |
| 14 | dbi.c:828/829 → :847 | dbi_rename_locked | — | SAFE (wave 3) |
| 15 | table.c:135/136 → :138 | tbl_create | — | SAFE (wave 3) |
| 16 | tree-ops.c:196-206, 345-353, 378-386, 766-769, 782-785, 813-816, 831-834, 848-851, 864-867 | node_move / page_merge temporaries | :782-785 (numkeys==0 second merge branch) **was not in the wave-3 list**; adjacent pairing, single `page_merge` between | SAFE (**NEW** site 782-785) |
| 17 | tree-cutoff.c:238/239 → :356 | cutoff_zikkurat dozer | — | SAFE (wave 3) |
| 18 | tree-cutoff.c:587-590 | cutoff dupsort tail-trim (head = clone of `end`) | none (adjacent; only `cutoff_storey` between); registers the clone couple's *outer* keyed on the *inner* cursor's dbi | SAFE (**NEW**) |
| 19 | tree-cutoff.c:616-619 | cutoff dupsort head-trim (tail = clone of `begin`) | none (adjacent) | SAFE (**NEW**) |
| 20 | page-split.c:293/294 → :296 | page_split parent-split | none (adjacent, single recursive `page_split`) | SAFE (**NEW**) |
| 21 | txn-basal.c:286/287 → :304 | txn_basal_update_tbl_roots gc cursor | — | SAFE (wave 2) |
| 22 | gc-put.c:1422/1423 → :1546 (`bailout`) | gc_update ctx cursor | every error `goto bailout`; `retry`/`continue` only; no `return` inside the loop | SAFE (**NEW**) — long-lived registration spanning the whole gc_update, restored at the single bailout label |
| 23 | txn.c:88/89 | txn_shadow_cursors (nested begin) | persistent shadow registration; failure path (cursor_shadow error, :82-86) returns with earlier shadows still registered — cleaned by `txn_done_cursors` at nested end incl. failed begin (wave 2 INV-T territory) | SAFE (**NEW** vs wave-3 list) |

Sweep verdict: **1 violation total (V-037, known)**; 4 SAFE-A with explicit
assumptions; everything else SAFE. Non-registration sites (`txn->cursors[i] =
nullptr` in txn.c:53-55/262-263/414, dbi.c:85; dangling-DBI guards dbi.c:103/
370/453; walks in cursor-setup.c:99, spill.c:119, tree-ops.c:282/307/537/645/
684, cursor-mut.c:986/1038/1067/1107, page-ops.c:259, page-split.c:503,
logging_and_debug.c:460, chk-walk.c:687) need no pairing.

### 1.2 env lifecycle (pair 2)

* **`mdbx_env_create` ladder** (api-env.c:211-265): each step frees exactly
  what earlier steps allocated — dbi_lock fail → free env; remap_lock fail →
  destroy dbi_lock + free env; wrt_lock stub fail → destroy remap_lock +
  dbi_lock + free env (POSIX `sem_init`/pthread-init failure leaves nothing
  to destroy — SAFE-A, platform ABI). VALGRIND mempool created only after all
  init succeeded.
* **`mdbx_env_openW` bailout** (api-env.c:512-567): single `bailout` label
  delegates to `env_close(env, false)`; `env_close` frees pathname.buffer
  (:656), kvs + names (:637-643), page_auxbuf, dbi_seqs, dbs_flags
  (:644-655) and destroys basal_txn (:660-663). `saved_me_flags` is restored
  so a failed open leaves the env retryable; on `env_close` failure the env
  is flagged `ENV_FATAL_ERROR` + `MDBX_PANIC` (L8).
* **`env_close` ordering** (env.c:559-667): txkey → munlock_all (lck still
  valid) → `rthc_remove` → `lck_destroy` (drowns ipc only under exclusive
  flock with no in-process neighbor and lck file still linked — the
  multi-process rule; last closer truncates the lck file) → ior destroy →
  `env->lck = nullptr` → lck/dxb munmap → events/pathname_char (Windows) →
  dsync/lazy/lck fd close → **`env->fd4meta = INVALID_HANDLE_VALUE`** (the
  #450-family fix, comment env.c:624-629: fd4meta aliases lazy/dsync and
  must be invalidated after both close, else a failed-open retry leaves it
  dangling into `lck_unlock`'s UnlockFile) → kvs/auxbuf/seqs/flags/pathname/
  basal_txn free. `mdbx_env_close_ex` (api-env.c:609-690) adds: signature
  CAS (single-close enforcement), basal-owner BUSY check, forced sync, the
  writer-mutex release for a checked-out basal txn, and the shadow_reserve
  drain. CONSISTENT.
* **`env_sync` ownership** (env.c:60-208): `env_owned_wrtxn` distinguishes
  own-txn / unlocked-caller / should_unlock paths; pre-sync failure sets
  `ENV_FATAL_ERROR` (L8) before any meta write; the presync retry re-reads
  under the writer lock; every exit passes `bailout` → `lck_txn_unlock`.
* **`mdbx_env_set_geometry`** (api-env.c:993-1369): pre-open path stages
  `geo_in_bytes` with ENSURE_OBJ bounds and `env_setup_pagesize`;
  post-open path takes the writer lock unless the caller owns the txn,
  all failures `goto bailout` (unlock), grow/shrink via `dxb_resize`,
  no-txn commit via `dxb_sync_locked` with a bumped txnid, and the
  deferred-shrink reconciliation re-reads `meta.geometry` after the sync
  (wave-2 dxb side). Units are bytes on both sides of the staging
  (`pgno2bytes`/`bytes2pgno`, grow/shrink round-tripped through
  `pages2pv(pv2pages(...))` with ENSURE_OBJ round-trip checks
  :1289-1293). CONSISTENT.
* **`mdbx_env_{set,get}_option`** (api-opts.c:201-640): `non_auto` bits are
  set exactly where a user value overrides a default (`dp_limit`,
  `rp_augment_limit`, `prefault_write`); `env_options_adjust_dp_limit`
  (api-opts.c:160-176) clamps a non-auto dp_limit down to
  `max_pgno - NUM_METAS` and **then** up to `CURSOR_STACK_SIZE * 4` — on
  micro-maps (V-014) dp_limit may therefore exceed the map's page count by
  design (release-safe, assert-tripping in checking builds; see
  `fact:verification:low-findings`). Deferred adjustment:
  `env_options_adjust_defaults` sets `need_dp_limit_adjust` when
  `env->txn` is live and `txn_basal.c:184` applies it at the next basal
  begin — the user-facing `MDBX_opt_txn_dp_limit` setter rejects
  `env->txn` with EPERM (api-opts.c:338), so the deferral is only
  reachable from internal geometry paths (dxb-resize.c:319). CONSISTENT
  (document the V-014 clamp order in module-interfaces.md).
* **Multi-thread note (CONSISTENT-A):** `mdbx_env_close_ex` checks
  `basal_txn->owner` before the signature CAS; a concurrent `txn_begin` in
  another thread between the check and the CAS is excluded only by the
  documented "do not close while other threads use the env" discipline.

### 1.3 txn-ro flag contracts (pair 3)

* **`check_txn` bad_bits semantics** (cogs.h:436-456): the
  `MDBX_TXN_BLOCKED - X` idiom *removes* bits X from the rejected set; when
  a txn is PARKED and `bad_bits` contains PARKED,
  `txn_check_badbits_parked` (txn.c:372-388) accepts the call **only** for
  an `AUTOUNPARK`-parked txn with no other bad bits, and then performs the
  unpark *inside the check* (documented for `mdbx_env_info_ex` in its
  comment). Manual-park (no AUTOUNPARK) → `MDBX_BAD_TXN`.
* **`mdbx_txn_park`** (api-txn.c:149-163 + txn-ro.c:328-365): rejects
  FINISHED/HAS_CHILD/PARKED (BAD_TXN) and non-ro (EINVAL); without a reader
  slot (exclusive) parking is impossible → EPERM; slot integrity
  (pid/tid/txnid) → PROBLEM/BAD_RSLOT; **an errored txn degrades to
  `txn_ro_reset` and returns MDBX_SUCCESS with the txn FINISHED** —
  document: "park of a broken txn == reset".
* **`mdbx_txn_unpark`** (api-txn.c:165-185 + txn-ro.c:367-428): bad_bits
  excludes PARKED and ERROR; the unpark loop breaks on OUSTED/ERROR/moved
  mapping and falls into `txn_ro_reset` → returns `MDBX_OUSTED`
  (FINISHED) or `MDBX_BAD_TXN` (broken); `restart_if_ousted` restarts via
  `txn_ro_start` → `MDBX_RESULT_TRUE`. The `txn->ro.base !=
  env->dxb_mmap.base` check (:397-401) ousts parked readers across a
  remap/move.
* **`mdbx_txn_reset` after park** (api-txn.c:91-105 + txn-ro.c:102-141):
  `check_txn(txn, 0)` passes parked txns; `ro_slot_clean`'s parked branch
  un-parks, detects `MDBX_TID_TXN_OUSTED` → sets `MDBX_TXN_OUSTED`,
  restores slot tid/txnid to idle-pinned state. **Note:** `txn_ro_start`
  (:239) replaces `txn->flags` wholesale, so `MDBX_TXN_AUTOUNPARK` does
  not survive reset+begin — document that autounpark is a per-park choice.
* **`mdbx_txn_clone`** (api-txn.c:626-723 + txn-ro.c:430-517): source must
  be live non-dirty (recursed to the basal parent); the clone gets its own
  reader slot, copies PARKED/AUTOUNPARK from the origin and re-publishes
  the parked tid; ousting detection compares the clone slot's txnid with
  `txn_basis_snapshot(origin)` and `cached_oldest_txnid` (fail-closed use
  of cached-oldest, cf. PAIR-N10/V-034). Slot takeback is *deliberately
  duplicated*: txn_ro_clone:501-512 (on OUSTED/RETARDED, with
  `safe64_reset_compare` ABA guard) and mdbx_txn_clone bailout :706-716
  (on post-publish recheck failure, unconditional reset) — the comment at
  txn-ro.c:502-504 states the duplication is defensive; both clear
  snapshot fields and restore a parked tid to the clone owner. Publish/
  recheck is guarded by `MPROBE_FAULT(txn_clone_recheck)`. CONSISTENT.

### 1.4 `mdbx_env_info_ex` units and convergence (pair 3)

* `env_info` (api-env.c:869-886) double-reads `env_info_snap` until two
  consecutive snapshots compare equal (the two 16dot16 clocks are copied
  across iterations so they cannot prevent convergence); livelock is
  possible only under sustained commit churn — acceptable for an info API.
* All geometry fields are bytes on both sides: `pgno2bytes` at
  :799-812/1357-1361, pre-open staging `geo_in_bytes` at :766-770;
  `mi_unsync_volume`/`mi_autosync_threshold` likewise (:822, :828). Same
  units as the wave-2-verified dxb side. CONSISTENT.
* Not-open env (issue #171): zeroed info except geo_in_bytes staging,
  max_readers, pagesize (:761-777).
* Reader enumeration (:850-861) iterates `i < mi_numreaders` where
  `mi_numreaders = rdt_count(env, lck)` — the L2 clamp
  (internals.h:747-752: `rdt_length` from the externally-writable lock
  mapping clamped to `env->max_readers` at **every** read site, with the
  hostile-lock-file rationale in the comment). In-bounds by construction.

### 1.5 gc_info / cache machine (pair 4)

* **`mdbx_gc_info`** (api-gc.c:5-100): the visitor loop is bounded by the
  callback contract — any `iter_func` return other than `MDBX_RESULT_FALSE`
  breaks (the "#88 hang" termination); corrupted-GC and `glr.err` breaks
  carry MDBX_CORRUPTED/err out; `goto next_record` is forward-only; the
  registration (site #7) is paired. The `pgops.gc_prof` double-read
  do-while (:22-28) stabilizes the two counters. `gc_row_pnl` and
  `gc_check_keylen` clamp hostile record shapes (wave-1 clamp family).
  `mdbx_txn_break` (api-txn.c:107-116; this fork has no separate
  `mdbx_txn_break_gc`) raises MDBX_TXN_ERROR across the whole nested chain
  and pairs with the parked-txn completion contract of §1.3.
* **`mdbx_cache_get` status machine** (api-get-cached.c): enum order is
  ERROR(-3) < BEHIND(-2) < UNABLE(-1) < RACE(0) < DIRTY(1) < HIT(2) <
  CONFIRMED(3) < REFRESHED(4) (mdbx.h:5725-5790), so both numeric
  comparisons are the intended ones: `status > MDBX_CACHE_DIRTY` in
  `cache_fallback` (:39-41) strips stub-entry statuses (HIT/CONFIRMED/
  REFRESHED) leaving the bypass status BEHIND/UNABLE/RACE with the real
  entry untouched (the stub guarantees that), and `status >
  MDBX_CACHE_HIT` in the MT publication (:354) publishes exactly
  CONFIRMED/REFRESHED (never DIRTY — uncommitted; never HIT — nothing to
  publish). Transition preconditions:
  - HIT :71-85 requires `txn->txnid <= last_confirmed` and
    `offset != 0` (data) or exact-txnid NOTFOUND;
  - BEHIND :67-69 requires `txn->txnid < trunk_txnid` (entry newer than
    the snapshot; the cached offset may have been recycled);
  - UNABLE :87-95 requires `trunk < txn->txnid < last_confirmed` (the
    documented ABA hole; full search with downgrade);
  - CONFIRMED :133-146 requires `tbl_root_txnid(dbi) <= last_confirmed`
    — sound because any CoW change under the tree re-stamps the root
    (INV-P1 stamps);
  - REFRESHED :117-124/:292-304 requires the full search and
    `trunk <= committed_snapshot`; DIRTY :292-294 is the
    `trunk > committed` case returned *without* touching the entry;
  - RACE is produced only by the MT seqlock (:319-338 spin on
    `last_confirmed > MAX_TXNID` with 4-yield fallback; :358-361
    concurrent-publication detection).
  All transitions reference only `txn_basis_snapshot`/`front_txnid`/
  `tbl_root_txnid`/`page_get` primitives verified in waves 2-3.
  The multivalue TODO (:281-284, `MDBX_EMULTIVAL` for N_DUP) is the
  documented known limitation. CONSISTENT with one hardening note (F-C4).

---

## 2. libmdbx-invariants.md additions

* **INV-CC5 (complete registration table):** every `txn->cursors[...]`
  registration site of the tree is one of table §1.1 (23 rows, incl. 7 rows
  first enumerated in wave 4: api-dbi.c:88, api-misc.c:95, api-gc.c:51,
  cursor-setup.c:124, page-split.c:293, gc-put.c:1422, txn.c:88, and the
  previously unlisted tree-ops.c:782-785 / tree-cutoff.c:587/616). Load-
  bearing corollaries: (a) the only violation remains V-037; (b) the two
  SAFE-A head-assumption restores (api-cursor.c:917 unconditional,
  :982-984 conditional) are correct iff the intra-call registrations they
  interleave with (#17) stay paired — INV-CC4 review rule covers both.
* **INV-E1 (env lifecycle ladder):** every failure exit of
  create/open/close reaches a teardown that frees exactly the resources
  allocated so far; `env_close` invalidates `fd4meta` after both owner fds
  are closed; the `osal_mmap_t.base/lck` union makes any
  post-`osal_munmap` reuse of the mapping struct inert (base==lck==nullptr),
  which is what makes the double-munmap-shaped code of env.c:586-587 safe
  after lck-posix.c:514.
* **INV-T9 (parked-txn gate):** `check_txn` with `bad_bits` containing
  PARKED auto-unparks an AUTOUNPARK-parked txn inside the check and
  rejects a manual-parked txn with BAD_TXN; park of an errored txn
  degrades to reset (SUCCESS + FINISHED); unpark of an ousted/errored/
  moved-mapping parked txn resets and reports OUSTED/BAD_TXN;
  AUTOUNPARK does not survive reset+begin.
* **INV-CACHE1 (cache entry safety):** a cache entry is trusted for
  ordering fields (trunk ≤ last_confirmed enforced, :57) and for the
  seqlock protocol; the *offset* field is trusted without a release-mode
  bound check (F-C4) — the invariant "cached offsets are inside
  [0, pgno2bytes(first_unallocated))" holds for entries produced by
  cache_get (:297 assert + :300) but is unenforced for entries supplied by
  the caller.

## 3. Findings

| F-id | file:line | severity | description |
|---|---|---|---|
| F-C4 | api-get-cached.c:72-77, :139-140 | doc/low (hardening) | `cache_get` HIT/confirmed fast paths dereference `entry->offset` against `dxb_mmap.base` with no release-mode bound check (`is_inside_dxb_and_commited` is tASSERT-only). An entry corrupted or hand-forged by the user with a large offset returns data from outside the mapped region (in-process SEGV or garbage). Entries produced by the library always satisfy the bound (:297-303); the check is cheap (`offset < pgno2bytes(first_unallocated)`) and would convert user misuse from SEGV to EINVAL. No in-tree scenario; docs must state the entry is trusted. |

Refutations (hypotheses killed by this wave):

| R-id | file:line | description |
|---|---|---|
| R-C3 | env.c:586-587 vs lck-posix.c:514 | Hypothesized double-munmap of the lck mapping (lck_destroy munmaps; env_close's `if (env->lck_mmap.lck)` still sees a dangling pointer). Killed statically: `osal_mmap_t.base` and `.lck` are a **union** (osal.h:166-170), and posix `osal_munmap` nulls `base` (osal-mmap.c:158-161), hence `.lck` too — the :586 guard is false after any munmap. Killed dynamically: `repro/v4c-close-lck-munmap.c` + strace shows exactly one munmap of the lck mapping per close, zero `munmap(NULL, 0)` calls, clean pass. The env.c:586-587 site is the *other* path (fork/no-exclusive/Windows) and is needed there. |
| R-C4 | api-txn.c:167 vs txn-ro.c:370-373 | Hypothesized dead defensive branch (unpark returning MDBX_OUSTED for FINISHED\|OUSTED flags unreachable past the API gate). Downgraded to "defensive, consistent": the check_txn/txn_check_badbits_parked layer (cogs.h:436-456, txn.c:372-388) also routes parked txns of *internal* callers; the branch mirrors cogs.h:466-467 exactly. No scenario constructed; not a finding. |

## 4. Pair verdicts (summary)

| pair | verdict |
|---|---|
| 1 registration sweep | CONSISTENT except known V-037 (1 of 23); 4 SAFE-A assumptions logged |
| 2 env lifecycle | CONSISTENT (create/open/close/geometry/options/sync); CONSISTENT-A: close-vs-begin TOCTOU (documented discipline), POSIX lock-stub init cleanup (platform ABI) |
| 3 park/unpark/clone/info | CONSISTENT; CONSISTENT-A: env_info convergence under sustained churn (livelock theoretical) |
| 4 gc_info/cache/readers | CONSISTENT; F-C4 hardening note |

## 5. test-coverage.md gaps

1. No test covers `mdbx_txn_clone` of a **parked** origin (PARKED/AUTOUNPARK
   flag copy + clone-slot parking + OUSTED detection path :492-512); the
   existing clone tests cover live/unparked sources.
2. No test drives `mdbx_cache_get` MT RACE path (needs NOSTICKYTHREADS +
   two threads racing on one entry — MPROBE `cache_check_txn_err` etc. are
   unused by the suite).
3. No test asserts `mdbx_env_info_ex` on a not-yet-open env returns the
   staged `geo_in_bytes` (issue #171 default behavior) after
   `mdbx_env_set_geometry` pre-open.
4. No test for `mdbx_cursor_bunch_delete`/`mdbx_cursor_delete_range` under
   MPROBE fault injection at `tree_cutoff_range` internals (would exercise
   the SAFE-A head-assumption restores of sites #11/#12).
5. `mdbx_env_close_ex(dont_sync=false)` with a live write txn of the *same*
   thread (the :628-657 forced-sync + unlock path) has no coverage; with
   CHECKOWNER the :656 branch is reachable only when the user abandons the
   txn handle — document as UB-adjacent or test explicitly.

## 6. KB keys written this wave

`fact:verification:api-surface-v4c` (this wave's registry),
`bug:api-get-cached:offset-unbounded` (F-C4), links to
`fn:cache_get`, `fn:txn_ro_park`, `fn:txn_ro_unpark`, `fn:txn_ro_clone`,
`fn:env_close`, `fn:mdbx_env_set_geometry`, `fn:rdt_count`.
Reused: V-037/INV-CC2..CC4, V-045, V-043, PAIR-N10, V-034, V-014, INV-P1,
INV-T5/T8, L1..L8.
