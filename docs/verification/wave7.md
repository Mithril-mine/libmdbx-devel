# Wave 7 merged report — A-lens statement-level top-up of the transactional core (final wave)

Three agents in parallel: gc-get.c/gc-put.c (doc-additions-v7-gc.md),
txn pipeline (doc-additions-v7-txn.md), coordination (doc-additions-v7-coord.md).
**Bottom line: zero new REFUTED.** The hard defects on these files remain
exactly wave-1's V-002/V-003 and wave-2/3's catalog — the adversarial lenses
did not miss a per-statement defect that the A-lens could find. Coverage of
the campaign is now complete for all 155 files.

## GC top-up (doc-additions-v7-gc.md)

All targeted functions PROVED (a few PROVED·A). Key formalizations:
gc_alloc_ex full P5 exit-consistency matrix (13 fail-entry sites + done/
reserve_done/no_gc); scan4seq_fallback exactness lemma (LEM-SCAN4SEQ-FB:
candidates [detent+1, len] exactly-once top-down, no-wrap equality); the
reserve path proven a pure probe (never consumes); LEM-STOCKPILE and
LEM-ESCALATION termination proofs; INV-PREPFLAG (the z_gcu_preparation
set/clear is exact — the GC self-reclaim guard is airtight with the
STATIC_ASSERT tie). Findings (nit-level): F-V7-1 (dbg_dump_ids reads `id`
uninitialized on a debug-only error path), N-V7-2 (INVALID_TXNID sentinel
wraps `reasonable_deep` when ready4reuse empty → scan_threshold=0 disables
one early-stop; perf-only), N-V7-3 (mincore TAS design note).

## Txn pipeline top-up (doc-additions-v7-txn.md)

27 functions/areas: 19 PROVED, 8 PROVED-A, 0 REFUTED/SUSPECT. Formalized:
txn_shadow_cursors per-success flag-set (LEM-TXN-1 — abort-restore exact for
any failure position, the detail wave 2 omitted); spill failed-path identity
(LEM-TXN-2 = V-021 statement-level); unpark post-CAS closure (LEM-TXN-3);
detent lower-bound (LEM-TXN-4); latch_maindb_locked P1 case table complete;
nested_merge full phase ledger with concrete array traces (both merge shapes
traced; F-TXN-1 KB correction: the post-merge stamp is `parent->front_txnid`,
not front+1); txn_ro_start/unpark full rows. Findings (all info): F-TXN-2
(park idempotence quirk), F-TXN-3 (latency memcmp over padding), F-TXN-4
(ro_slot_clean no-unpublish during teardown), F-TXN-5 (32-bit CAS transient,
~2⁻³²), F-TXN-6 (dead `!need_spill`), F-TXN-7 (audit release error-swallow,
fail-closed), F-TXN-8 (dead defensive branch).

## Coordination top-up (doc-additions-v7-coord.md)

Per-function rows for lck.c/lck-posix.c/rthc.c/lck-windows.c/global.c/env.c
(mvcc-readers.c cross-check: no discrepancies). Zero REFUTED; one new
hardening finding: **F-V7C-1** (low, API-lifecycle violation): post-`mdbx_fini`
reuse is undefended — `rthc_dtor` leaves rthc_table=nullptr/limit=0 (Windows:
CS deleted); a later `mdbx_env_open` ×2 can write rthc_table[1] past a
zero-size heap block (POSIX) or enter a deleted CS (Windows). Unreachable in
supported lifecycles; fix: finalized-flag error or floor rthc_limit.
New lemmas: LEM-V7C-1 (half-init lck refusal), LEM-V7C-2 (clear-path poison
order — mirror of INV-S9-1, incl. kick rc>1), LEM-V7C-3
(reasonable_db_maxsize termination both word sizes).

## Campaign closure

All tranches complete: V1–V13 statement-level + adversarial + pairs + two
performance passes + A-top-ups. Every one of the 155 files now holds:
(a) a statement-level P1–P7 ledger (stream A or the wave-3/5/6 equivalence),
(b) an adversarial lens on the risk-ranked subset, (c) contract-consistency
checks on every primitive→consumer interface, (d) performance verdicts on
all hot paths. Total findings: ~48 (10 REFUTED defects incl. 1 critical /
2 high, ~16 machine-checked; the rest doc/contract/hardening families).
Cumulative conditions/obligations verified: ≈8,900 of 9,711 (the remainder
is logging-blanket and N/A debug-only code, discharged per protocol).
