# Verification wave 10, stream B (adversarial, defrag + chk + foundation) — proposed doc additions

Scope: `src/defrag.c`, `src/gc.h` (dfc_t), `src/api-gc.c` (mdbx_env_defrag /
mdbx_gc_info), `src/dml.{c,h}` (arcs map), `src/chk.c`, `src/chk-walk.c`,
`src/chk-report.c`, `src/chk-internal.h` (#360 accounting), `src/walk.c` /
`src/walk.h` (walk consumer contract, S12 area), `src/pnl.{c,h}`
(pnl_get_best_sequence / pnl_scan_span), `src/atomics-ops.h`,
`src/atomics-types.h` (safe64, mo_*), `src/logging_and_debug.{c,h}`
(panic/ENSURE/bad_page), `src/env.c` (ENV_FATAL, env_owned_wrtxn),
`src/txn-basal.c` (checkpoint/restart), `src/utils.{c,h}`, `src/unaligned.h`,
`src/cogs.{c,h}` (bounds), `src/comparators.{c,h}`, `src/preface.h`
(platform gates), `src/options.h` (defaults sweep).

Verdict protocol as in LEDGER.md. Machine-checked repros live in
`docs/verification/repro/v10b-defrag-{cycles,timelimit,adopt-cursor}.c`
(built against a Release static lib; every run ended with `mdbx_chk`
reporting no error).

Reused prior results (not re-proven): L3/V-001 (troika dead proof),
LEM-V3-6 (page_check min-space), LEM-V4-4 (walk accounting exactness),
V-053d (walk depth contract / consumer caps), A7 (default builds trust DB
contents — crafted-data defenses are validation/chk-only), V-039
(page_split bound), detent≥seq lemma, INV-C1 (walk containment chain),
V-045 (INTEGERKEY align4 pin), INV-S9-1 (bind poison-publish).

---

## 1. Primitive contracts (S12 area additions for `libmdbx-invariants.md`)

### INV-D1 — defrag arcs-map accounting (the dont_walk_MAIN alternation)

`dfc->arcs` is a forest mirroring tree ancestry: an arc's `parent` is the
tree-parent pgno (updated per move by the cycle-2+ remap pre-pass,
defrag.c:867-875), so parent chains are acyclic and their length is bounded
by the walker depth cap (32). The map holds **exactly one arc per payload
page** and one per currently-observed GC-tree page above `walk_cutoff`
because of a deliberate walk-options alternation:

* cycle 1 (`payload_items == 0`): `walk_pages(..., dont_check_keys_ordering)`
  walks GC + MAIN (+ all named tables through MAIN's N_TREE nodes) and pushes
  every page `>= walk_cutoff`, tagging arcs `is_gc` iff
  `table->internal == &dbs[FREE_DBI]`;
* the cycle-2+ pre-pass **removes all GC arcs** ("will be re-loaded") and
  keeps every payload arc with `key_or_pgno = mapped` (defrag.c:882-912);
* cycle 2+ walks with `dont_walk_MAIN`, which skips the top-level MAIN
  `walk_tbl` (walk.c:394) — and named tables are only reachable through
  MAIN — so the only fresh pushes are the current GC-tree pages.

Attack results: within one walk, a crafted DAG/cycle is refused on the spot
by `walk_visit`'s revisit bitmap (defrag does NOT pass `dont_check_revisit`,
walk.c:45-65); across cycles the alternation makes duplicates impossible, so
neither the turn loop (adjacency check) nor the LP loop can process the same
page twice. The `walk_stack[32]` cap is a plain release check
(defrag.c:296-297, `MDBX_CORRUPTED`), which also bounds
`defrag_schedule`'s parent-chain walk (`stack_of_parents[CURSOR_STACK_SIZE*2]
= [64]`, chains ≤ 32). `DEFRAG_TRACK_FLAG = 0x80000000 > MAX_PAGENO` is
guarded by the STATIC_ASSERT at defrag.c:301.

### INV-D2 — defrag moves only touch free or moving pages

Every move destination is cut from `repnl` (free pages) before use
(`defrag_repnl_get` / `pnl_get_best_sequence`, whose contiguity scan cannot
return a span ending at/above `first_unallocated`); every source is either in
the arcs map (payload, referenced by exactly one parent) or read-only-copied.
Consequently even the residual hardening gap below (targets potentially above
`retreat_edge`, see F-V10-3) cannot overlap a live source page, and a
suboptimal LP target is self-healing (the misplaced large page is re-arc'd
next cycle; span depletion bounds the loops).

### INV-C2 — chk problem accounting (the #360 ledger)

Every problem and every declined scope is counted exactly once:
* `chk_scope_begin_args()` counts a declined/failed `scope_push` into the
  caller's per-stage counter and immediately merges it into
  `result.total_problems` (chk.c:143-155), leaving `scope_depth` unchanged;
* `chk_line_begin()` counts any line at severity < warning
  (chk-report.c:16-19) — this also covers the `chk_object_issue()` malloc
  failure path, which additionally logs via `chk_error_rc()`;
* `chk_scope_end()` merges the current per-stage counter into the total only
  at stage boundaries and only `if (counter != &total)` — the same-pointer
  guard makes double-counting impossible, and an error with zero problems
  raises the counter to 1 (chk.c:92-100);
* per-stage pointers are never switched mid-stage in the actual call graph
  (chk.c:443/608/643/727/742; chk-walk.c:616 re-arms the same `&problems_kv`
  inside the same stage), so no stage's subtotal is dropped from the total;
* a top-level `MDBX_RESULT_TRUE` is escalated to `MDBX_PROBLEM`
  (chk.c:860-862), and a declined tree traversal cascades correctly: the
  decline is counted into `tree_problems`, which is propagated to
  `gc_tree_problems`/`kv_tree_problems` so GC/MAIN processing is skipped with
  the skip counted as problems too (chk.c:615-618, 638-641, 722).

### INV-C3 — chk GC-record containment (extends the wave-3 clamp verification)

For each GC record, the raw leading count `number` is read only when the
record is at least one `pgno_t` long and 8-aligned (chk-walk.c:713); it is
clamped — with `clamped = true` — whenever `number > PAGELIST_LIMIT` or
`(number+1)*sizeof(pgno_t) > iov_len` (:715-726). Both consumers are bounded
by the clamped value: the per-entry walk reads `pnl[1..number]` inside the
record, and the span-details pass (`pnl_maxspan`/`pnl_scan_span`) is skipped
for clamped records via `tbl->cookie && !clamped` (:773). For unclamped
records `pnl_scan_span` iterates `while (from+span <= pnl_size(pnl))` with
`pnl_size == number` (pnl.h:211-218), so even an "extra idl space" record
(gap allowed up to 2 pages, :726-732) cannot be over-scanned. `usr->result.
gc_pages/reclaimable_pages` accumulate the clamped number only.

---

## 2. Findings

| id | area | severity | verdict | summary |
|----|------|----------|---------|---------|
| V-060 | defrag API (#55) | doc/contract, minor | CONFIRMED (machine-checked) | Stuck-retry stop reports `rc=MDBX_SUCCESS` + `stopping_reasons=noobstacles(0)` despite a named obstacle; residual work is visible only via `pages_left`. |
| V-061 | defrag API (#55) | doc/contract, moderate | REFUTED vs docs (machine-checked) | `time_limit_dot16` is evaluated only after the `defrag_atleast` goal edge is reached (`defrag_progress` gate, defrag.c:86-87); with an unreachable goal a 15 µs limit was ignored through 25 cycles / 3 ms of work. |
| V-062 | defrag adoption (#306) | doc/contract, minor | CONFIRMED (machine-checked) | Adopting the caller's write txn makes each cycle's `txn_basal_checkpoint()` commit the user's pending writes mid-txn (proved by abort-survival) and detaches the user's cursors; subsequent `mdbx_cursor_get()` safely returns `MDBX_EINVAL` (drown → `cur_signature_ready4dispose`, `cursor_check` → EINVAL). Behavior is safe and chk-clean but undocumented in mdbx.h. |
| F-V10-1 | defrag score | hardening | HELD (attack failed) | Stop/commit gates use `defrag_score` whose span bonus can decline across a productive cycle; every `defrag_cycle` exit either sets `stumble_pgno` (bounded by the retry counter) or returns non-SUCCESS (breaks the API loop), so score oscillation cannot loop the API loop. |
| F-V10-2 | defrag liveness | hardening | HELD (bounded) | `stumble_retry` resets whenever the stumble pgno changes; a zero-progress livelock would need ≥ 2 alternating obstacles with zero moves and identical high-arc states — not constructible in the runs (9 and 25 cycles, retry bounded at 4); residual risk noted for review profiles. |
| F-V10-3 | pnl_get_best_sequence callers | hardening | HELD (self-healing) | `pnl[i] <= defrag_detent - seq` underflows when `defrag_enough < seq` (possible: one huge large page vs tiny `payload_pages`); the filter weakens and a target may land above `retreat_edge`. INV-D2 keeps it corruption-free; debug ASSERTs (pnl.c:356, defrag.c:571/594/605) document the intended bound. Add `defrag_enough >= seq` guards at the two direct callers (defrag.c:978, :1069). |
| F-V10-4 | bad_page/poor_page dedup | info | HELD (benign) | `static const page_t *prev` log-once state is cross-thread racy; it only gates one summary log line, the detail line is unconditional, and the return value `MDBX_CORRUPTED` is unaffected — worst case duplicate/suppressed log noise. |
| F-V10-5 | chk pgvisitor npages | info | HELD | Crafted `lp.page->pages` up to `UINT32_MAX` cannot OOB: the per-page loop bounds every `pagemap[spanpgno]` access by `spanpgno >= alloc_pages` (chk-walk.c:156-172); a 32-bit `pgno + n` wrap only mis-classifies a page as "wrong page-no". |

### Machine-checked evidence (Release static lib, per run `mdbx_chk` clean)

* **v10b-defrag-cycles.c** — fragmented main+named tables, `preferred_batch=400`,
  no user txn: 9 cycles, `pages_moved=783`, `pages_shrinked=771`,
  `left=12`, ends via `stucked at 795` bailout with `rc=MDBX_SUCCESS`,
  `stopping_reasons=0x0` (V-060); all 6000 named-table records intact;
  post-run `mdbx_chk -n`: no error.
* **v10b-defrag-timelimit.c** — same shape, `time_limit=1` (≈15 µs),
  unreachable `defrag_atleast=600`: all 25 cycles ran to the stuck stop,
  `stopping_reasons=0x0`, no `MDBX_defrag_time_limit` bit (V-061).
* **v10b-defrag-adopt-cursor.c** — user write txn + open cursor + pending
  put, then `mdbx_env_defrag()`: cursor refused with `MDBX_EINVAL` after the
  defrag; the pending put **survived `mdbx_txn_abort()`** (it was committed
  by the cycle checkpoint) (V-062); `mdbx_chk -n`: no error.

---

## 3. Defended attacks (why the defenses hold)

1. **walk_stack depth vs 32** (V-053d family): enforced by a release
   `if` (not ASSERT) returning `MDBX_CORRUPTED`; sub-table recursion (N_TREE,
   N_TREE|N_DUP) increments the same `deep`, so crafted nesting is capped
   too; chains ≤ 32 < `stack_of_parents[64]`.
2. **Arcs-map cycles**: `parent` mirrors tree ancestry, remapped only to
   `p->mapped ? p->mapped : p->key_or_pgno` of an existing arc; a cycle would
   require two pages to be each other's tree parent — impossible; the chain
   walk additionally terminates at `!chain->parent` (root arc has parent 0).
3. **remapped_edge/retreat_edge oscillation**: monotone non-decreasing
   (`remapped_edge = max(...)`); `retreat_edge` is per-cycle constant;
   `defrag_edge` strictly decreases inside the turn loop (each iteration
   assigns `i->key_or_pgno` ≤ previous `defrag_edge - npages` via the
   adjacency check). Loop exits: stumble (bounded), step_size, discontinued,
   arcs exhausted — all terminate.
4. **defrag_score as a cycle-termination signal**: see F-V10-1; the score is
   a heuristic only — every termination path is independently bounded.
5. **#82 crafted exponential walk on defrag**: `walk_visit` revisit bitmap +
   `nwalked > walk_limit` (first_unallocated) + the 32-deep cap — three
   independent bounds; defrag does not disable them.
6. **safe64 protocol**: `safe64_reset` poisons to `UINT64_MAX` (≥
   `SAFE64_INVALID_THRESHOLD = 0xFFFFFFFF00000000`) before any publish;
   `safe64_reset_compare` non-CAS path does CAS(high) + low re-check with an
   undo CAS predicated on its own poisoned value (ABA guarded by the ±1 low
   bumps around multi-writer resets, `xMDBX_TXNID_STEP > 1` enforced by
   STATIC_ASSERT at atomics-ops.h:374); `safe64_txnid_next` overflow ends in
   `MDBX_TXN_FULL` at `MAX_TXNID` (txn-basal.c:169-173), never wraps into the
   poisoned range.
7. **atomics mo_* mapping**: `mo_AcquireRelease` lowers to
   `memory_order_acquire` on loads and `memory_order_release` on stores
   (atomics-types.h:161-168) — no AcquireRelease downgrade; non-C11
   non-gcc/apple/MSVC compilers hit unconditional `#error FIXME atomic-ops`
   (atomics-types.h:87) and `#error FIXME: Unsupported compiler` in every
   CAS/add helper — the gate is total. MSVC non-x86 without C11 atomics is
   rejected up front (atomics-ops.h:45-47).
8. **ENSURE/panic contract**: `ENSURE*` always panics (never error-returns);
   `panic_internal` calls the user `panic_func` (if any) and then
   unconditionally `osal_panic()` (logging_and_debug.c:312-321) — a returning
   or re-entering custom panic_func cannot prevent termination.
9. **cogs bounds at ps=256 / ps_min**: `BRANCH_NODE_MAX(256)=112`,
   `LEAF_NODE_MAX(256)=118`, both even, `> NODESIZE+42` /
   `> sizeof(tree_t)+NODESIZE+42` (ENSUREs env.c:46-48); the degenerate-page
   cases are excluded by STATIC_ASSERTs at the MIN_PAGESIZE (cogs.h:63,
   68-70, 84-86).
10. **comparators**: INTEGERKEY/INTEGERDUP values are compared via
    `cmp_uint_align4`/`cmp_uint_unaligned` only (builtin_datacmp,
    comparators.h:255-263); user-supplied misaligned 4/8-byte keys are copied
    into `alignkey_t::intbuf` by `check_key` (cogs.h:250-270) — this is the
    release-side bcopy path; on-page alignment rests on the V-045 pin, and
    for `expected_alignment=4` with 8-byte values the `unaligned_peek_u64`
    32-bit split path is taken on strict-alignment targets (unaligned.h:
    130-144). No misaligned reach was constructible.
11. **`defrag_enough - npages` unsigned filter in `defrag_provide_span`**:
    underflow only weakens the outer scan filter; the inner span detection
    still requires `pnl[i-span] < defrag_enough`, and the paranoia check
    (:741) rejects any candidate not strictly below both edges.
12. **chk declined meta scope**: early return counted and escalated
    (chk.c:595-600, 860-862); the subsequent stages are skipped rather than
    run on degraded values.
13. **unclamped forged tree_t consumers**: all height/pages/items consumers
    either report mismatches as issues (chk_pgvisitor :111-124, chk_db
    :490-506) or are release-validated at bind time (`tbl_fetch`: height ≥
    CURSOR_STACK_SIZE → CORRUPTED, mod_txnid > page txnid → CORRUPTED);
    no unclamped arithmetic consumer exists in the chk path.

## 4. Options sweep (defaults and enforcement)

Every sampled option has at least one enforcement/consumption site; no
"default that is never enforced" was found:
`MDBX_DPL_CACHE_NPAGES` (gates `dp::npages`, internals.h:77-79; the =0 path
is the long-standing no-cache layout of the verified DPL ladder),
`MDBX_ENABLE_PROFGC` (counter-only instrumentation at gc-get/gc-put/
mvcc-readers/defrag sites; writer-side single-writer increments), 
`MDBX_DEBUG_GCU` (gc-put logging macros), `MDBX_ENABLE_FAKE_NESTED_
READONLY_TRANSACTIONS` (api-txn.c:297/316, txn.c:102/175, txn-nested.c:741/
750), `MDBX_TRUST_RTC` (osal-bootid.c:285/219), `MDBX_NOSUCCESS_PURE_COMMIT`
(txn-basal.c:383), `MDBX_CHECK_MEMZEROLEN` (utils.h memcpy/memcmp_checklen —
the V-005 family). `MDBX_UNALIGNED_OK < 8` on x86-64 is safe: meta fields use
`expected_alignment=4` and take the well-defined 32-bit-split path
(unaligned.h:169-178), consistent with the wave-2 A-3 result.
`preface.h` platform gates: exactly one hard `#error FIXME` (alignas
exhaustion, preface.h:305) — a total compiler-feature gate; the second
`FIXME` hit (preface.h:970) is a message prefix macro, not a gate.

## 5. Test-coverage gaps

1. No test drives `mdbx_env_defrag()` to the stuck-retry stop and asserts the
   `stopping_reasons`/rc contract (V-060 would fail today).
2. No test checks the time-limit contract with an unreachable `defrag_atleast`
   (V-061).
3. The adopted-txn path (#306) has only the data-loss regression
   (issue_gh0306); no test pins the mid-txn checkpoint-commit semantics or
   the cursor-refusal behavior (V-062).
4. `pnl_get_best_sequence` detent-underflow (F-V10-3) has no CHECKING-build
   assertion test (the debug ASSERTs fire only if a contiguous span exists).
5. chk problem-accounting (#360): no test declines scopes via custom
   callbacks and asserts the exact `total_problems` ledger.
