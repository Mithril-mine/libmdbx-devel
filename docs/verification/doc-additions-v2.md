# doc-additions-v2 — proposal edits from verification tranche V2 «durability/format»

Stream A (bottom-up), statement-level verification of `layout-dxb.h`,
`layout-lck.h`, `meta.h`, `meta.c`, `dxb.c`, `dxb-resize.c`, `coherency.c`,
`histogram.c`. Full ledger report: coordinator report (this tranche), machine
probe: `docs/verification/repro/v2_durability_probe.c`.
Nothing below is applied — proposals only, for the coordinator to validate and
merge into the canonical docs.

---

## 1. libmdbx-invariants.md — new invariants (§3 numbering continues at 8)

### 8. Two-phase meta discipline is reader-safe under every write transport
The pair (readers via `meta_snap()`, writers via any of: `meta_update_begin`/
`meta_update_end` in-place WRITEMAP update, whole-`meta_t` `pwrite` in the
non-WRITEMAP path, whole-page `memcpy` in `meta_override()` under exclusivity)
preserves "a reader sees the whole old meta or the whole new meta, or a meta
that fails the `txnid_a == txnid_b` gate". Proof sketch (tranche V2, stream A):
- the writer's store order is always `txnid_b ← 0/old`, `txnid_a ← new`,
  payload, `txnid_b ← new`; a torn or partially-visible `txnid_a` can only
  equal a concurrently-read `txnid_b` if `new ≡ old (mod 2³²)` — impossible
  for the monotone `xMDBX_TXNID_STEP ∈ {1,2}` increments;
- on amd64/e2k the 64-bit store path is hardware-atomic because `txnid_a`
  (page offset 28) and `txnid_b` (page offset 196) each lie wholly inside one
  64-byte cacheline for every pagesize ≥ 64;
- a false-INVALID outcome (transient `txnid == 0`) is benign — every consumer
  treats `txnid < MIN_TXNID` as "not usable" (`meta_validate`,
  `meta_snap` steadiness gate, `meta_eq`).
Evidence: `fact:verification:two-phase-reader-safety` in the KB;
`repro/v2_durability_probe.c` PROBE-1.

### 9. pv/pages packing lemmas (`pages2pv`/`pv2pages`, cogs.c)
- **D∘E∘D = D**: `pv2pages(pages2pv(pv2pages(pv))) == pv2pages(pv)` for all
  65536 `pv` — machine-checked at every startup by `pv2pages_verify()`
  (global.c:298; NOT dead code, contrast V-001) and re-proved by
  `repro/v2_durability_probe.c` PROBE-2.
- **E∘D idempotent**: `pages2pv(pv2pages(·))` applied twice is the identity on
  its own image; consequence: `meta_model()`'s ENSURE
  `grow_pv == pages2pv(pv2pages(grow_pv))` (meta.c:314-315) holds for
  grow/shrink values from EVERY source — `mdbx_env_set_geometry`
  normalization (api-env.c:1221), geometry read back from a (foreign or
  crafted) meta, and the `dxb_setup()` amendment path.
- **Round-up**: `pv2pages(pages2pv(v)) ≥ v` for `v < me2v(2046,7)`; above that
  the value clamps down to `me2v(2046,7)` (pv 65533). The packed space is NOT
  injective: `me2v(119,1) == me2v(239,0) == 94208`; `pages2pv` canonicalizes
  to the minimal exponent.

### 10. meta_validate geometry correction chain preserves ordering
After `meta_validate()` returns MDBX_SUCCESS:
`MIN_PAGENO ≤ lower ≤ upper ≤ MAX_PAGENO+1`,
`first_unallocated ≤ now ≤ upper`, and every correction (32-bit clamping of
`lower`/`upper`, `now` pulled into `[lower…upper] ∪ filesize`) mutates only
the caller-owned buffer (the header copy in `dxb_read_header`, the recovery
clone in `meta_validate_copy`, the model page in `meta_override`) — never a
live meta page. All 14 reject branches are enumerated and each predicate
verified in the tranche-V2 ledger (`fact:verification:meta-validate-branches`).

### 11. Shrink arithmetic never cuts live data
`dxb_shrink_maybe()`/`dxb_shrink_bottom()` (dxb.c:779-804) use strict
inequalities on the REAL file size, so `filesize_pgno − first_unallocated` and
`filesize_pgno − largest_pgno` cannot underflow; `bottom ≥ largest ≥
first_unallocated` (the aligner only rounds up), so a shrink target never
drops allocated pages; `largest + stockpile_gap + aligner < 2³²` so the `pgno_t`
arithmetic cannot wrap; the zero return value is a non-colliding "nothing to
shrink" sentinel because `bottom ≥ lower ≥ NUM_METAS = 3`
(`fact:verification:shrink-two-phase-arithmetic`).

### 12. The #269 validate-after-read protocol
`coherency_fetch_head()` copies trees/geometry/canary from a live meta page and
only then re-validated by `txn->txnid != meta_txnid(head.ptr_v)`. Soundness: a
torn copy implies an in-flight single-writer update which either is still
in flight at the re-snap (gate fails, a≠b) or has completed (txnid differs
from the pre-tap `head.txnid`). Either way the copy is rejected; the loop
bails out through `coherency_timeout()` after ≈100 ms
(`osal_16dot16_to_monotime(65536/10)` = 6553·2⁻¹⁶ s — one tenth of a second,
not 6.5 s) with MDBX_PROBLEM (`fact:verification:fetch-head-validate-after-read`).

---

## 2. subsystem-map.md / module-interfaces.md — primitive contracts

### meta.h / meta.c
- `meta_snap(meta)` → `(txnid, is_steady)`: post `txnid == 0 ∨ txnid ≥ MIN_TXNID`;
  `txnid == 0` ⟺ the meta is mid-update, torn or legacy-invalid. All consumers
  MUST treat `txnid == 0` as "not usable" (they do).
- `meta_update_begin(env, meta, txnid)`: pre `meta` inside the triplet,
  `constmeta_txnid(meta) < txnid`; post `txnid_a == txnid ∧ txnid_b == 0`
  (observer-invisible intermediate states excluded per invariant 8).
- `meta_update_end(env, meta, txnid)`: pre `txnid_a == txnid ∧ txnid_b == 0`;
  post `txnid_a == txnid_b == txnid`, bootid stamped.
- `constmeta_txnid(meta)` → `txnid_a == txnid_b ? a : 0` — for images built by
  `meta_set_txnid()` the result is the real txnid; for live pages it is 0
  exactly while an update is in flight.
- `meta_tap(env)` → `troika.fsm ∈ [0, 215]`, one of the 216 table entries;
  `recent/prefer_steady/tail` are the lex-max/lex-max/remainder with the
  documented tie rule "exact tie → the later meta index wins" (tie states are
  exactly the `meta_eq`-clashed states, unreachable while single-writer holds).
- `meta_validate(env, meta, page, n, &guess)` — pure w.r.t. the env EXCEPT the
  filesize re-stat race path, which refreshes `env->dxb_mmap.filesize`;
  geometry fields of `meta` may be corrected in place (never a live page,
  see invariant 10). Returns one of: SUCCESS, MDBX_RESULT_TRUE (skip:
  mid-update / bad steady sign / txnid range), MDBX_INVALID,
  MDBX_VERSION_MISMATCH, MDBX_INCOMPATIBLE, MDBX_CORRUPTED, MDBX_TOO_LARGE,
  propagated stat error.

### dxb.c
- `dxb_sync_locked(env, flags, pending, troika)`: pre — writer lock held,
  `pending->geometry.first_unallocated ≤ pending->geometry.now`,
  `pending` not inside the meta triplet; post — on SUCCESS the pending meta is
  durable (steady) or deliberately weak (`MDBX_SAFE_NOSYNC`/`NOMETASYNC`),
  `*troika` re-tapped, nested txns' troikas refreshed; on FAILURE
  `ENV_FATAL_ERROR` is set — with the single documented exception of the
  discard-scan madvise path (finding V-025 below).
- `dxb_shrink_maybe(pending, filesize_pgno)` / `dxb_shrink_bottom(...)`: pure
  predicates over the real file size; contract in invariant 11. A deferred
  shrink keeps `meta.geometry.now < dxb_mmap.filesize` and is re-attempted by
  a later commit — this transient is regular, not an error.
- `dxb_read_header(...)`: on SUCCESS `dest` holds a validated meta whose
  pagesize was used to set `env->ps` by the caller; known blind spot V-024.

### dxb-resize.c
- `dxb_resize(env, allocated, size, limit, mresize_flags)`: on
  `MDBX_EPERM`/`MDBX_UNABLE_EXTEND_MAPSIZE` the mapping and env state are
  unchanged (deferred), the env stays usable; `MDBX_PANIC` only when the
  mapping was lost. Windows lock order is always
  `resize_mutex → remap_lock`, released in reverse; readers only ever hold
  `remap_lock` shared (txn.c:345), so no cycle exists.
- `dxb_set_readahead(env, edge, enable, force)`: advisory only; every error
  path returns before the anchor update, so the anchor may lag — harmless.

### coherency.c
- `coherency_check_written(env, txnid, meta, pgno, &timestamp)`: returns
  MDBX_SUCCESS / MDBX_RESULT_TRUE (retry) / MDBX_PROBLEM (bailout after
  ≈100 ms). The timestamp argument must be a zero-initialized per-call-site
  local; NULL means "no retries".

### histogram.c
- `histogram_acc_ex(v, p, options)`: slots stay packed `ranges[0..k-1]` full,
  `ranges[k..last]` empty; `finder` is only invoked when ALL slots are full
  (precondition of its `count > 0` asserts); the merge loop terminates after
  at most one `histogram_reduce_move` per insertion. Purely a reporting
  primitive — wrapping of `amount`/`count` counters and of the 64-bit
  heuristic error metric is benign.

---

## 3. test-coverage.md — gaps and proposed tests

1. **V-001 obligation (re-affirmed)**: call `troika_verify_fsm()` from a unit
   test (MDBX_CHECKING build). PROBE-1 in `repro/v2_durability_probe.c`
   re-proves the FSM with an independent tournament + structural-tail
   recomputation, but in-tree the proof still never executes.
2. **V-023**: test that `mdbx_env_set_geometry()` with a large growth step
   (e.g. 4 GiB) and pagesize 65536, then reopen in a 32-bit build (or a
   32-bit-emulating CI cell), keeps the grow step intact or fails loudly
   instead of silently wrapping to 0/distorted values (dxb.c:354-355).
3. **V-024**: white-box test for `dxb_read_header()`: file with
   pagesize > sys_pagesize, meta-0 damaged (torn `magic_and_version`),
   meta-1/meta-2 intact — open must succeed from a valid meta (currently
   returns MDBX_CORRUPTED). Worst case worth fixing cheaply by probing the
   second pass at `2^k`-scaled offsets or by re-reading meta-0's pagesize
   field even when other checks fail.
4. **V-025**: fault-injection test (MPROBE-style) for madvise failure
   (non-EINVAL) inside the `dxb_sync_locked()` discard scan asserting the
   documented contract: whichever of "ENV_FATAL_ERROR is set" (current L8) or
   "discard failure is non-fatal" is chosen, it must be stated and tested;
   today the code and the LEDGER lemma disagree.
5. **Sign stub**: when `meta_sign_calculate()` gets a real implementation,
   add a characterization test that a corrupted steady meta is rejected by
   `meta_validate()` (today the steady-checksum branch is vacuous — the stub
   returns `~0` for everything).
6. **Deferred-shrink reconciliation**: a Windows-targeted (or simulated)
   scenario asserting `mi_geo.current == txn->geo.now * pagesize` while a
   shrink is deferred (the `env->geo_in_bytes.now = pgno2bytes(pending->now)`
   path, dxb.c:1211).
7. **pv packing**: `pv2pages_verify()` is already called at startup; consider
   ALSO asserting the `E∘D` idempotence (Lemma 9) there — it is the property
   meta_model's ENSUREs rely on, currently only implied.

---

## 4. Findings referenced above (V-ids continue the global registry)

| V-id | Where | Essence | Severity |
|---|---|---|---|
| V-023 | dxb.c:354-355 | 32-bit builds: `pv2pages(grow_pv) * pagesize` / `…shrink_pv * pagesize` wrap mod 2³² (grow/shrink pages are never bounded by meta_validate; 65536 pages × 65536 = 0; me2v(2047,7) × 4096 = 134217728 instead of 275052437504) — silent geometry distortion or spurious open failure for cross-ABI files | low |
| V-024 | dxb.c:52-55 | pagesize discovery: without a valid meta-0 (offset 0 is the only pagesize-independent position), meta-1/2 are probed at sys_pagesize multiples and never found when db pagesize > sys_pagesize → MDBX_CORRUPTED despite 2 intact metas | low |
| V-025 | dxb.c:959 | dxb_sync_locked discard-scan madvise failure (non-EINVAL) returns directly, bypassing `fail:` — ENV_FATAL_ERROR not set, refuting LEDGER lemma L8 as stated | low |
| V-026 | meta.c:195 | meta_troika_dump prints tail as `tail_and_flags % 3`; the strict/valid bits leak (64≡1, 128≡2 mod 3) — wrong tail digit whenever flags set; log-only | info |
| V-027 | dxb-resize.c:475 | `readahead_anchor = (enable&1) + (edge<<1)` overflows pgno_t at edge = 2³¹ (pagesize 256, ≥ 512 GiB file) → anchor loses the edge; advisory degradation only | info |
