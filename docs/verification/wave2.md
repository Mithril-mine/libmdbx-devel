# Wave 2 merged report — V2 durability/format (A), V6 txn pipeline (B, adversarial), pairs-2 (C)

Branch `verification/master-audit`. Per-stream full ledgers: stream B —
`doc-additions-v6-b.md`; stream C — `doc-additions-v2c.md`; stream A — below.
Repro harnesses: `repro/v2_durability_probe.c` (PROBE-1/2 PASS, PROBE-3/4
reproduce counterexamples), `repro/v6b-txnid-dist-magic.c` (576 sequences +
crash injection, passes), `repro/v6b-layout-drift-check.c`, 
`repro/v6b-nested-begin-fault-recipe.c`.

**Registry note:** streams independently used intersecting V-ids; canonical
renumbering applied here (KB keys are slugs and unaffected). Canonical:
A→V-023..V-027, B→V-028..V-030, C→V-031..V-036.

---

## Stream A — V2 durability/format (meta/dxb/coherency/histogram/layout-*)

Verdict census: layout-dxb.h, layout-lck.h, histogram.c — PROVED (blanket +
per-function); meta.h/meta.c — PROVED (meta_validate: **all 14 reject/skip
branches enumerated and verified**; the issue-#454 equivalences confirmed
complete); coherency.c — PROVED·A (the #269 validate-after-read protocol
proven sound, 100 ms bailout = 6553×16.16); dxb.c — PROVED·A with 3 findings;
dxb-resize.c — PROVED·A (lock order `resize_mutex → remap_lock` everywhere —
no cycle; no blocking acquire — the #154 deadlock is gone by design).

Full per-function ledger preserved in `doc-additions-v2.md` (proposals) and
summarized here; the durability core results:

### dxb_sync_locked (dxb.c:806–1226) — PROVED·A
Complete exit-map enumerated: every failure sets ENV_FATAL_ERROR **except**
the discard-scan madvise early-return (V-025). Auto-sync gating bit algebra
correct (force-steady keeps txn_mresize_flags). Shrink block: `bottom ≥
largest ≥ fua` (never cuts live data), zero-sentinel collision-free,
force-forward under `ENSURE !env->txn`. WRITEMAP meta update = two-phase
begin/end + fence + msync; non-WRITEMAP = pwrite + undo-on-failure.
Deferred-shrink reconciliation (geo_in_bytes.now ↔ meta) verified P5.
meta_sync_txnid ring arithmetic verified for both endiannesses.

### meta_validate (meta.c:416–699) — PROVED (14 branches)
magic/pagesize/pgno/flags/a≠b/sign/gc.flags/table-flags/txnid-range/
lower/upper/fua/filesize/mmap_limit; geometry correction chain preserves
`lower ≤ upper`, `fua ≤ now ≤ upper`; 32-bit clamps case-checked; #454
height/root/leaf/branch equivalences complete; dead branch after branch 11
noted (defense-in-depth).

### New lemmas (stream A)
`two-phase-reader-safety` (all 3 write transports; false-valid requires
`new ≡ old (mod 2³²)`; amd64 path justified by cacheline containment —
txnid_a@28, txnid_b@196); `pv-pages-roundtrip` (D∘E∘D=D machine-checked at
global.c:298; E∘D idempotent); `meta-validate-branches`; `sync-locked-exit-map`;
`fetch-head-protocol`; `shrink-two-phase-arith`; `troika-tie-rule` (full tie ⇒
later index; tie states = clashed, unreachable); `histogram-packing`.

### Stream A findings
| V-id | file:line | severity | essence |
|---|---|---|---|
| V-023 | dxb.c:354–355 | low | 32-bit `size_t` wrap in geometry-from-db `pv2pages(grow/shrink_pv)·pagesize` (unbounded by meta_validate) — probe PROBE-3 |
| V-024 | dxb.c:52–55 | low | pagesize discovery blindspot: damaged meta-0 + db pagesize > sys_pagesize ⇒ valid meta-1/2 never probed ⇒ CORRUPTED with 2 intact metas — probe PROBE-4 |
| V-025 | dxb.c:959 | low | discard-scan madvise failure exits without ENV_FATAL_ERROR — **L8 refuted as stated** (exception registered) |
| V-026 | meta.c:195 | info | troika dump prints tail as `%3`: flag bits leak (64≡1, 128≡2 mod 3) — log-only |
| V-027 | dxb-resize.c:475 | info | `readahead_anchor` overflows pgno_t at edge=2³¹ — advisory degradation |

---

## Stream B — V6 txn pipeline (adversarial)

Verdict census: every function ATTACKED→held or NOT-ATTACKED; **no
critical/high refutations**. Highlights:

- **#448 verified complete** (INV-T1 `nested-begin-fault`): geo/dbi_seqs/
  dbi_sparse snapshot precedes the first fallible alloc; field-state table
  per failure site S1..S8 proves every abort assert holds.
- `txn_shadow_cursors` error path — REFUTATION FAILED (INV-T2): the
  DBI_OLDEN|DBI_LINDO mark dies with the nested handle; the parent's cursors
  chain is reconstructible via backups; malloc-before-mutation verified.
- `nested_join` "No further failures" claim — TRUE except the tail
  `dbi_update` (V-028): all reservations verified arithmetically (INV-T3).
- `txn_basal_commit`: **no fallible ops between `txn_write` success and
  `dxb_sync_locked`** (INV-T5) — half-written pages are unreachable garbage
  until the meta flip; every pre-sync failure aborts cleanly.
- `txnid_dist` NOMETASYNC magic reconciled machine-checked (INV-T6, repro).
- `txn_ro_unpark` base-ABA: POSIX move vetoed while any local slot holds a
  valid txnid; Windows same-base remap is the design outcome; moved-base
  detection load-bearing (INV-T8).
- `txn_refund` overlap scenarios: alternation loop refunds correctly or
  conservatively skips; `dpl_setlen` does NOT clamp `sorted` — pinned as
  INV-T9 (initial dead-code suspicion refuted).
- `audit_ex` = page-ledger balance identity (INV-T10): detects
  double-allocation and unledgered pages; trusts maintained stats (model gap
  documented).
- R7 layout drift: **REFUTED as an active bug** (INV-T7) — `txn_alloc` never
  builds a basal txn; hazard is re-use only (hardening recommendation stands).

### Stream B findings
| V-id | file:line | severity | essence |
|---|---|---|---|
| V-028 | dbi.c:273 + txn-nested.c:701 + txn-basal.c:271 | low | `dbi_update` early-returns on `dbi_lock` failure before `dbi_rename_undo_discharge`: nested commit leaks the rename-undo chain; basal abort leaves the undo on the reused basal txn (not cleared by `basal_start_locked`) to be discharged under the wrong commit flag. Mutex-failure gated; state consistent |
| V-029 | txn.c:392 vs txn-basal.c:66 | info | layout-drift **risk refuted** for every flags/max_dbi combination; re-use hazard + hardening documented |
| V-030 | txn.c:164–173 | info | abort latency snapshot loop vs shared `pgops` — theoretically unbounded, practically bounded |

---

## Stream C — pairs-2

All four pairs CONSISTENT·A; per-site enumeration tables in
`doc-additions-v2c.md`. Highlights: unsafe_txnid raw reads are consumed only
behind the double-read+validate gate (fact `unsafe-meta-read`); the CHECKING
0xCC provoke memset is unobservable by readers (copy-at-begin + re-tap);
`legal4overwrite` memcmp set sufficient by construction for all 4 sign-only
callers; nested WRITEMAP prohibition (txn-nested.c:458-461) is the
load-bearing gate of the page-state machine (PAIR-N9); page_split root-split
stack bound (PAIR-N7); 14 cursor ops sampled against the §5 outcomes table —
3 uniformity gaps found (V-032/V-033/V-035); forged cached-oldest makes RO
opens fatal (V-034); kick/detent cycle proven terminating (PAIR-N10);
`txn_refund` writer-local, cannot shrink below a live snapshot (PAIR-N11).

### Stream C findings
| V-id | file:line | severity | essence |
|---|---|---|---|
| V-031 | dxb.c:590 (+389-395, 640-644) | low | bootid-match branch of dxb_setup drops user-amended geometry (reopens after unclean shutdown lose the requested upper/grow) |
| V-032 | cursor-seek.c:100+115 vs 152-159/280 | low | SET_RANGE past-end has two encodings (ki=nkeys−1 via sibling-failure vs ki=nkeys fast path): GET_CURRENT diverges (last row vs NOTFOUND) — R1 gap, matches the §5 table note |
| V-033 | cursor-seek.c:516-517 | low/doc | TO_KEY_EQUAL miss leaves the cursor filled on the neighbor when SET_RANGE landed on one — table row covers only the past-end case |
| V-034 | txn-ro.c:195/121/284; txn-basal.c:253 | low | forged-high cached_oldest_txnid: RW recovers via rescan, RO fires release-fatal ENSURE (reader-visible DoS until a writer heals the cache) |
| V-035 | cursor-seek.c:676, 844 | low/info | `cursor_distance_at` eof_hard rank adjustment assumes ki=nkeys−1; the ki=nkeys encoding wraps size_t distance negative — release-safe, trips ASSERT in checking builds |
| V-036 | mvcc-readers.c:394-502 | info | mvcc_kick_laggards declining-hsr loop INT_MAX-bounded but practically unbounded (V-004 family) |

### New invariants (stream C)
PAIR-N7 (root-split stash bound), PAIR-N8 (page_get ILL dispatch early-return),
PAIR-N9 (front_txnid page-state machine + nested-WRITEMAP prohibition),
PAIR-N10 (cached-oldest distrust discipline + kick termination),
PAIR-N11 (tail-refund reader safety).

---

## Cross-stream checks (wave 2)

- Stream C's `legal4overwrite`-sufficiency analysis and stream A's
  `two-phase-reader-safety` agree (same transports, same conclusion).
- Stream B's INV-T9 (`dpl_setlen` non-clamping) cross-confirms stream A's
  dpl.h:8-22 verdict and explains refund accounting.
- V-025 (A) vs L8: LEDGER L8 amended with the documented exception.
- V-036 is family-consistent with V-004 (both liveness, fail-safe, no
  correctness impact).
- No verdict contradictions.

## KB records (streams wrote directly; Store keys, normalized)

A: `bug:dxb:geo-wrap`, `bug:dxb:pagesize-discovery-blindspot`,
`bug:dxb:sync-locked-exception`, `bug:meta:troika-dump-tail`,
`bug:dxb:readahead-anchor-overflow`, `fact:verification:two-phase-reader`,
`fact:verification:pv-pages-roundtrip`, `fact:verification:meta-validate-branches`,
`fact:verification:sync-locked-exit`, `fact:verification:fetch-head-protocol`,
`fact:verification:shrink-two-phase`, `fact:verification:troika-tie-rule`,
`fact:verification:histogram-packing` + 14 links to fn: symbols.
B: `bug:txn:dbi-update-lock` (V-028), `fact:verification:nested-begin-fault`,
`cursor-shadow-restore`, `nested-join-infallible`, `basal-commit-failure`,
`nometasync-txnid-dist`, `txn-layout-drift`, `unpark-base-aba`,
`dpl-setlen-no-clamp`, `audit-identity-model`, `spill-lsb-abort`,
`info-findings` + 12 cbm_link bridges.
C: `bug:meta:dxb-bootid-geo`, `bug:cursor:set-range-past`,
`bug:cursor:key-equal-miss`, `bug:mvcc:cached-oldest-forged`,
`bug:cursor:distance-eof-hard`, `bottleneck:mvcc:hsr-decline-loop`,
`fact:verification:page-split-stash`, `page-get-ill`, `front-txnid-state`,
`cached-oldest-discipline`, `tail-refund-reader`, `unsafe-meta-read` + links.

## Metrics after wave 2

- Functions with verdicts: 93 (w1) + ~60 (A) + ~35 (B) + ~45 (C-site groups) ≈ 230+
- Conditions verified: ~1,300 (w1) + ~1,100 ≈ **~2,400 / 9,711**
- Cumulative REFUTED: 6 (V-002 critical, V-003 high, V-004 medium, V-023/24/25 low) + refuted-as-non-bug risks (V-029/R7)
- Open assumptions: ~25 (each = obligation)
