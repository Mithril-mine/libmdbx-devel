# Wave 4 merged report — V4 tree (A), S9 coordination adversarial (B), API-surface pairs (C)

Branch `verification/master-audit` @ `9ad900f4`. Full per-stream details:
stream A ledger below (doc-additions-v4.md); stream B — doc-additions-v8-b.md;
stream C — doc-additions-v4c.md. Repro: `repro/v4-dump-position-overshoot.c`
(machine-checked, 3 configurations), `repro/v8b-lckless-reader-mvcc.c`
(machine-checked attack-10 probe), `repro/v4c-close-lck-munmap.c` (strace-
verified). Registry: canonical V-049..V-053 (continuing wave-3's V-048).

---

## Stream A — V4 tree (statement-level) — PROVED·A throughout, zero REFUTED defects

The cleanest tranche so far. Highlights: tree_search descent FSM and the
branchless search cycles proven exact lower_bounds (LEM-V4-1, `it += cmp&1`
restores the answer); tree_rebalance's root-collapse bookkeeping proven
incl. the duplicate-tail release-safety; node_move/page_merge ki arithmetic
exact (LEM-V4-2: node 0 = don't-care placeholder, position-0 refresh sites
load-bearing); walk_pgno byte accounting exact for all five page shapes
(LEM-V4-4); dbi.c state machine internals verified (sparse-bit walk
wrap-safety, the one seq-first publication window reconfirmed, the
rename-undo LIFO/prepend semantics incl. same-dbi A→B→C); tree_cutoff
zikkurat region discipline (INV-V4-Z). **V-038 scope closed**: with
BUNCHES=0 the build fails exactly at tree-ops.c:25/:87 (the cursor_pop
rename) and `-Dcursor_pop=cursor_eject` compiles clean — nothing else in the
`#else` branches is broken; their runtime logic verified statement-level.

### Stream A findings (all minor/info)
| V-id | where | severity | essence |
|---|---|---|---|
| V-053a | tree-cutoff.c:8-39 cursor_dump_position | debug-only, latent | `tail += snprintf(...)` uses the untruncated return → wrap after first truncation; not reachable today (all 13 call sites pass pageptr=false); machine-checked repro (fortify SIGABRT / tail=1921 / FIXED exit 0) |
| V-053b | dbi.c:284-285 | cosmetic | dead publish branch (filter excludes it) |
| V-053c | dbi.c fastpath vs slowpath | minor inconsistency | bind-failure closes the env slot only in the slowpath — race-dependent outcome, both recoverable |
| V-053d | walk.c recursion | robustness note | C-stack depth bounded only by the visitor (all current consumers cap: chk 42, api-copy deep_limit, defrag walk_stack) — contract must be documented |
| V-053e | table.c tbl_stat_summary | consistency note | scan cursors lack z_pagecheck in default builds vs walk_tbl's unconditional arming (A7-consistent) |

New lemmas: LEM-V4-1..V4-5, INV-V4-Z; assumptions A-V4-1..3 registered.

---

## Stream B — S9 coordination (adversarial) — no REFUTED; 10 attacks: 8 held, 1 machine-checked, 1 premise-disproven

Key held results: bind poison-before-publish is leak-free against crash
mid-publish (INV-S9-1); the non-atomic `rdt_length.weak = ++nreaders` is the
single writer site, mutex-serialized, gate-bounded — no torn/lost-update
counterexample (INV-S9-2); pid-reuse degrades to liveness only (INV-S9-3);
lckless gate is ALL-bits and machine-checked — a writable env never goes
lckless on POSIX (INV-S9-4, repro E2/E3); POSIX2008 EOWNERDEAD recovery
checks ALL readers before `pthread_mutex_consistent` (the #21031-class
correctness); fork child isolation (INV-S9-5); all ten `#error FIXME` sites
are compile-time flavor guards, autodetection never selects uncovered
flavors (INV-S9-6); OFD↔classic downgrade/upgrade races covered by split
semantics + rollback; SysV ftok collision reclaim-vs-flee bounded (8
attempts, ctime re-check).

### Stream B findings
| V-id | where | severity | essence |
|---|---|---|---|
| V-049 | mdbx.h:1180-1183 vs osal-fs.c:15-28 | low/doc | lckless promised for "directory and/or files not writable", actually only ST_RDONLY honored — docs broader than code |
| V-050 | mvcc-readers.c:282-376 vs :461 | low | cleanup_dead lacks the kick's `pid ≤ 0` guard: crafted negative pid → fcntl EINVAL → persistent bind failures (bounded liveness DoS from a hostile lock file; no unsafety). Fix: clear pid ≤ 0 slots as stale |
| V-051 | mdbx.h:7459-7540 vs lck-posix.c:988-1027 | low/doc | HSR callback re-entrancy contract undocumented: POSIX2001/2008 fail-safe EDEADLK, POSIX1988/SysV hang, Windows likely re-entry |
| V-052 | lck-posix.c:285-417 | low (by-model) | restored-copy lck replacement + two-opener race can reach the shared-return seize without the dxb exclusive → two coordination worlds → concurrent writers (silent corruption); fresh replacement defused by the 2 s deadline. Windows items verified by reading only (no Windows CI) |

---

## Stream C — API surface pairs — CONSISTENT except known V-037; 23 registration sites swept

**Pair 1 (INV-CC4 mechanical sweep)**: complete table of **23 registration
sites** of the `txn->cursors[dbi]` idiom across src/ — 7 newly enumerated
beyond the wave-3 fact (api-dbi.c:88, api-misc.c:95, api-gc.c:51,
cursor-setup.c:124, page-split.c:293, gc-put.c:1422, txn.c:88). The only
violation remains V-037. Four SAFE-A sites with logged assumptions
(bunch_delete unconditional head-restore; delete_range conditional; gc_info
no-longjmp contract).

**Pair 2 (env lifecycle)**: create/open/close ladders verified
statement-level (INV-E1); **R-C3 refuted dynamically** — the hypothesized
double-munmap (env.c:586-587 after lck-posix.c:514) is impossible:
`osal_mmap_t.base/.lck` are a union, osal_munmap nulls both (strace repro:
zero `munmap(NULL,0)`); lck_destroy multi-process drown rules verified;
V-014 reconfirmed (adjust_dp_limit intentionally exceeds map pages on
micro-maps).

**Pair 3 (park/unpark/clone/info)**: the parked-txn gate decoded — new
**INV-T9** (check_txn auto-unparks only AUTOUNPARK-parked txns inside the
check; AUTOUNPARK does not survive reset+begin; park of an errored txn
degrades to reset); mdbx_txn_clone's duplicated slot takeback verified
ABA-guarded + unconditional bailout; env_info geometry units converge
(bytes, double-read).

**Pair 4 (gc_info/cache/readers)**: cache 7-status machine verified (enum
order makes both `> DIRTY` and `> HIT` filters correct; CONFIRMED soundness
rests on INV-P1 root re-stamping; BEHIND gate blocks recycled offsets; no
cached-oldest trust on the HIT path). New finding **V-054**: `cache_get`
HIT/confirmed paths dereference `entry->offset` against the map base with
no release-mode bound check (assert-only) — a forged entry yields out-of-map
reads; O(1) clamp would close it (doc/low hardening).

New invariants: INV-CC5 (complete registration table), INV-E1, INV-T9,
INV-CACHE1. KB: `fact:verification:api-surface`,
`bug:api-get-cached:offset-unbounded` + links.

---

## Cross-stream checks (wave 4)

- C's INV-CC4 sweep and B's S9 ledger: the txn.c:88 shadow-cursor
  registration (persistent, cleaned by txn_done_cursors) cross-confirmed by
  wave-2's INV-T2 restore semantics — no contradiction.
- A's V-053d (walk depth contract) matches C's wave-3 consumer caps (chk 42,
  api-copy deep_limit, defrag walk_stack) — consistent.
- B's V-052 remains the only by-model corruption-class residual (Windows
  reading only, low likelihood).
- No verdict contradictions.

## Metrics after wave 4

- Functions with verdicts: ≈320 (w3) + ~70 (A tree) + ~45 (B coordination) + 4 pair-groups + 23-site sweep ≈ **440+**
- Conditions verified: ≈3,300 + ~1,100 ≈ **~4,400 / 9,711**
- Cumulative REFUTED defects: 10 (V-002 critical, V-003 high, V-037 high, V-004/V-038 medium, V-023/24/25/39/53 low-family) + refuted-as-non-bug risks (R7/V-029, V-038-batch, R-C1/R-C2/R-C3)
- Tranches complete: V1, V2, V3, V4, V5, V6(B-lens), V7(B-lens), V8(B-lens), pairs-1..4. Remaining: V9 api statement-level, V10 foundation/defrag/chk, V11 osal, V12 tools, V13 C++, plus A-lens top-ups for gc/txn/coordination.
