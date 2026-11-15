# doc-additions-v9 — tranche V9 (stream A, API surface: statement-level argument/option validation)

Statement-level verification of every `src/api-*.c` function (14 files, 8132
lines): argument validation completeness, option set/get matrix, flag
decoding, resource ladders, and logging arithmetic. This tranche REUSES and
does not re-open: INV-CC4/CC5 (registration sweep, 23 sites — V-037 the only
violation, confirmed still present at api-txn-data.c:301), INV-E1 (env
ladders), INV-T9 (parked gate), V-014 (micro-map dp_limit), V-043/V-037
(estimate divergence family), V-045 (clc pin stickiness), V-054/INV-CACHE1
(cache offset unenforced), V-034/PAIR-N10 (cached-oldest), mdbx_env_copy
#450, INV-S9-* (wave 8). New code-level finding: **F-V9-1 (sync_bytes
uint32 truncation, machine-confirmed, `repro/v9-api-opts-syncbytes-truncation.c`)**.
One hypothesis (amend TRY+PARKED re-park) was **machine-REFUTED** — the
repro is kept as a regression test
(`repro/v9-txn-amend-park-try.c`).

Method note: verdicts are per-function; "PROVED-A" marks reasoning that
rests on a stated assumption (A-ids below). Logging arithmetic (monotime
diffs, ratio formatting, estimate histograms) verified once per file as a
blanket.

---

## 1. Contracts worth adding to `docs/engineering/module-interfaces.md`

### 1.1 Option set/get matrix (`mdbx_env_set_option` / `mdbx_env_get_option`, api-opts.c)

Every `MDBX_opt_*` case verified: domain check → write; `UINT64_MAX`
sentinel → per-option default; get is the exact read of the stored field.
The full matrix (option | set validation | default sentinel | get | class):

| option | set validation | `UINT64_MAX` means | get | class |
|---|---|---|---|---|
| sync_bytes | RDONLY→EACCESS, !ACTIVE→EPERM, `value > SIZE_MAX-65536`→EINVAL; then `bytes2pgno` stored as **`(uint32_t)` with NO page-count check** | MAX_IO_BYTES | `pgno2bytes(autosync_threshold)` | **F-V9-1** |
| sync_period | RDONLY/EACCESS, ACTIVE/EPERM, `> UINT32_MAX`→EINVAL | 42.42424 s | monotime→16dot16 | PROVED |
| max_db | `> MDBX_MAX_DBI`→EINVAL; mapped→EPERM | 42 | `max_dbi - CORE_DBS` | PROVED |
| max_readers | `<1 || > MDBX_READERS_LIMIT`→EINVAL; mapped→EPERM | READERS_LIMIT | direct | PROVED |
| dp_reserve_limit | `> INT_MAX`→EINVAL; shrink loop frees surplus under txn lock | default | direct | PROVED (free loop mirrors close_ex) |
| rp_augment_limit | `> PAGELIST_LIMIT`→EINVAL; non_auto bit maintained | recompute-default | direct | PROVED |
| gc_time_limit | `> UINT32_MAX`→EINVAL, RDONLY→EACCESS, in-txn→EPERM; recomputes rp_augment default unless pinned | 0 (=off) | monotime→16dot16 | PROVED |
| txn_dp_limit/initial | `> PAGELIST_LIMIT \|\| < CURSOR_STACK_SIZE*4`→EINVAL, RDONLY→EACCESS, in-txn→EPERM; clamps per V-014 | auto | direct | PROVED (V-014 reuse) |
| spill_*_denominator ×3 | `> 255`→EINVAL (0 allowed) | per-option default | direct | PROVED — all consumers zero-guard (spill.c:179/183, txn-nested.c:470) |
| loose_limit | `> 255`→EINVAL | 64 | direct | PROVED |
| merge_threshold | `[8192, 32768]` else EINVAL; recalc | default | direct | PROVED |
| writethrough_threshold | POSIX: `value != (unsigned)value`→EINVAL; Windows: only sentinel or current-mode value | default | direct | PROVED |
| prefault_write_enable / prefer_waf | `> 1`→EINVAL; non_auto bit | recompute-default | direct | PROVED |
| subpage_* ×4 | `> 65535`→EINVAL; recalc on every accepted path | recompute-default | direct | PROVED |
| split_reserve | `> 32768`→EINVAL | 0 | direct | PROVED |
| presync_threshold | `!lck`→EPERM; `> 0x80000000`→EINVAL; result `max(ceil2pages,1)` | default | `lck ? pgno2bytes : 0` | PROVED |
| shrink_retry_period | RDONLY→EACCESS; `> UINT32_MAX`→EINVAL; 0 disables throttling | 5 s | monotime→16dot16 | PROVED |
| unknown | EINVAL | — | EINVAL | PROVED |

**F-V9-1 (bug:api-env:syncbytes-truncation, PROVED, machine-checked)**: the
`MDBX_opt_sync_bytes` case validates bytes but stores a page count that can
exceed 32 bits as `(uint32_t)`. `set(2^44)` on 4 KiB pages stores
`(uint32_t)2^32 = 0` — get returns 0, i.e. the user's threshold silently
becomes "sync at every transaction" (or disabled, per consumer reading of 0)
instead of 2^44. No UB, pure contract break; fix is an
`if (value > UINT32_MAX) return MDBX_EINVAL;` after `bytes2pgno`.
Reproducer: `docs/verification/repro/v9-api-opts-syncbytes-truncation.c`.

### 1.2 Argument-validation contract — the release "assert gate" inventory

For module-interfaces.md: which public entry points validate their handles
in release builds and which gate only on `ASSERT`/`tASSERT` (compiled out).
This is the defensive-asymmetry inventory (V-041/V-053e family, now
enumerated):

| API | release validation | contract status |
|---|---|---|
| `mdbx_cmp` / `mdbx_dcmp` (api-misc.c:109-121) | **none** (tASSERT only: signature, DBI_VALID, dbi bounds) | mdbx.h documents "Undefined behavior if one of arguments is invalid" — ACCEPTED-BY-DOC |
| `mdbx_double/float/int32/int64/jsonInteger_from_key` (api-key-transform.c:142-160) | **none** (ASSERT-only `iov_len == 8/4`) | docs bind the precondition ("The key made by mdbx_key_from_double()") but carry no explicit UB warning — **doc gap; recommend the same warning sentence as mdbx_cmp** |
| `mdbx_dbi_dupsort_depthmask` bit `1 << nested_height` (api-txn-data.c:41) | height read raw from the node; the release height check lives in tbl_fetch, which this path does not use | crafted DB + height ≥ 32 = UB shift; A7-consistent (bounded garbage bit, no memory unsafety) — A7-hardening list |
| `mdbx_env_get_userctx` (api-cold.c:453) | null-check only; no signature check — unlike `mdbx_env_get_hsr` (:466) which checks `env_signature` | dangling-env read possible; ACCEPTED (handle-lifetime contract), asymmetry worth aligning |
| everything else in api-*.c | full release validation (null, signature via check_env/check_txn/cursor_check, dbi_check, bytes/size gates) | PROVED |

### 1.3 Env/txn/cursor/data contracts confirmed at statement level

- `mdbx_env_open` mode filtering (api-env.c:458-568): unknown flag bits →
  EINVAL (`ENV_USABLE_FLAGS`); RDONLY silently strips
  WRITEMAP/MAPASYNC/SAFE_NOSYNC/NOMETASYNC/COALESCE/LIFORECLAIM/NOMEMINIT/ACCEDE
  and zeroes mode; durability flags combine with previously
  `mdbx_env_set_flags` values via `combine_durability_flags`; single bailout
  → `env_close(false)` restores saved flags (INV-E1 reuse). PROVED.
- `mdbx_env_set_geometry` (api-env.c:993-1369): every arithmetic clamp
  re-checked; the only unsigned wraps (`max_pgno - NUM_METAS`) are the
  known V-014 micro-map case; `ceil_powerof2` inputs are bounded by the
  preceding `mmap_limit`/`MAX_PAGENO` clamps; growth/shrink round-trips
  through `pages2pv` ENSUREd; the coherency loop (#269) and txnid-next
  overflow ladder (TXN_FULL) both end at `bailout` with the lock released.
  PROVED.
- `mdbx_txn_begin_ex` (api-txn.c:249-334): `env->flags & MDBX_RDONLY &
  ~flags` = write-txn-on-RO-env EACCESS; per-family flag masks
  (`txn_ro_begin_flags` / `txn_rw_begin_flags` / nested: only
  RDONLY|READWRITE); flat-RO parent → BAD_TXN; PREPARE asserts
  `txn->flags == (txn_ro_flat|FINISHED)` (debug only). PROVED.
- `mdbx_txn_commit_ex` idempotent finish: `BAD_TXN && FINISHED && ro_flat`
  → abort + RESULT_TRUE. `mdbx_txn_rollback` accepts ERRORED/PARKED
  (bad_bits = BLOCKED − ERROR − PARKED). PROVED (INV-T reuse).
- `mdbx_txn_amend` (api-txn.c:747-812): the TRY/PARKED symmetry compares
  full flag VALUES, so parking is attempted only in the no-TRY+not-parked
  case — machine-REFUTED hypothesis, see §3 R-V9-1. `txn_basal_start`
  failure releases the writer lock internally
  (txn-basal.c:234-239 calls `txn_basal_end(txn, true)` when
  `txn_rw_already_locked`) — no lock leak on the failure ladder. PROVED.
- `mdbx_txn_checkpoint` RESULT_TRUE short path (api-txn.c:449-450) returns
  without `txn_latency_done` — latency stays zeroed on that success path
  (F-V9-2, doc/consistency, low).
- `mdbx_env_info` double-read convergence (api-env.c:869-885): loops until
  two consecutive snapshots memcmp-equal — bounded per iteration, may spin
  under sustained concurrent commits (accepted design, upstream-inherited).
  The not-open branch (#171) `memset(out)` AFTER `env_info_sys` filled
  bootid/sys fields — those are reported as zero for a not-open env
  (geo/maxreaders/pagesize preserved); intentional per the #171 comment but
  worth a sentence in the module docs (F-V9-3, doc).
- `mdbx_env_close_ex` PID-check-before-CAS, sync-skip for deleted files
  (`st_nlink > 0`), shadow_reserve free loop — INV-E1 reuse, PROVED.
- `mdbx_preopen_snapinfo_ex(W)` (api-env.c:913-989): stack MDBX_env fully
  re-initialized (INVALID fds, sysv −1, options); mode passed through to
  `dxb_read_header`; only head-meta fields populated (mi_meta_sign[1..2]
  stay 0 — single-meta snapshot by design); `env_info_sys` on the bailout
  path always runs, `env_close` always frees the pathname buffer. PROVED.
- `mdbx_dbi_open*` null/pseudo-name handling: `MDBX_CHK_MAIN == NULL`
  (mdbx.h:7894), both the cstr wrapper (skips strlen on sentinel) and
  `dbi_open` (pointer-or-iov_base pseudo detection, dbi.c:665-670) treat
  NULL as MAIN without dereferencing; rename rejects pseudo names at both
  levels (api-dbi.c:128-134). PROVED (null-deref hypothesis refuted
  statically).
- `mdbx_drop` MAIN guard: `dbi_enumerate(named_table_probe)` refuses MAIN
  drop while any N_TREE record exists (#311/#69 family); sequence reset
  keeps DBI_DIRTY; del path registers its MAIN cursor at :88-91 paired with
  :91; SLAIN/LINDO vs immediate-close ladders per dbi_state. PROVED.
- `mdbx_dbi_close` (api-dbi.c:158-222): double n_dbi check (outside/inside
  lock), MAIN no-op, hazard loop with consumer fences — DANGLING_DBI on
  DIRTY/CREAT (LINDO-only passes); `dbi_close_release` releases the lock.
  PROVED (documented TOCTOU discipline).
- `mdbx_is_dirty` (api-txn-data.c:178-212): the four-region map (inside
  allocated / inside map beyond allocated / outside map WRITEMAP / outside
  map malloc) with page-header sanity gate returning EINVAL for
  mid-large-page pointers. PROVED.
- `mdbx_get_ex` / estimate single-key paths clamp `nested_tree.items` to
  PTRDIFF_MAX for 32-bit `size_t`. PROVED.
- `mdbx_cursor_bunch_delete`/`mdbx_cursor_delete_range` (SAFE-A sites of
  wave 4c) re-verified statement-level: every `get_axe`/clone registration
  is followed only by `break`s that reach the restore; `delete_range`
  never clones twice into one couple (both-null rejected up front);
  outer_first/outer_last failures return before registration. PROVED.
- `mdbx_cache_get` MT publish (api-get-cached.c:354-376): a sound
  seqlock — `safe64_reset_compare` poisons `last_confirmed` before the
  `trunk=0`/offset/length field writes and `safe64_write` unpublishes
  after, so a reader whose first/last reads of `last_confirmed` agree
  cannot observe the intermediate field states; monotonicity guard
  `snap >= local.last_confirmed → RACE`. Publishes only status > HIT
  (CONFIRMED/REFRESHED, never DIRTY). `cache_fallback` recursion is depth-1
  (stub entry has trunk=last_confirmed=0, so the fallback itself never
  re-enters the fallback branches). PROVED. V-054 (offset never
  bounds-checked: deref sites api-get-cached.c:74, :139, store :300)
  re-confirmed, not re-opened; `entry->length = (uint32_t)iov_len`
  truncation is unreachable for engine-validated values because
  `valsize_max` is hard-capped at `0x7FF00000` (cogs.h:110-127) — A7-only.
- `mdbx_gc_info` (api-gc.c): paired FREE_DBI registration (:51-52/:97);
  NOTFOUND on an empty GC table is returned as MDBX_NOTFOUND (empty-DB
  contract, matches mdbx.h); next-reader delta guarded against wrap by the
  `head_retired > snapshot_retired` gate plus the 5-field stability
  recheck; adaptive-batch multiply in `mdbx_env_defrag` can overflow u64
  for absurd time limits but is clamped to `[1024, payload_pages]`
  afterwards (perf-only). `mdbx_env_defrag` txn ladders PROVED: checkpoint
  failure ends the txn internally (txn-basal.c:505-531), so the
  `dfc.txn = txn = nullptr` bailout skip is correct; `defrag_init` memsets
  `dfc` before any early return, so `defrag_result`/`defrag_destroy` at the
  bailout label never read uninitialized stack.
- Reader/aux shells (api-extra.c): reader_list 5-field stability recheck +
  poison→0 mapping; thread_unregister clears pid only after verifying the
  slot txnid is already poisoned (INV-S9-1 order preserved); txn_lock/unlock
  gates (RDONLY EACCESS, FINISHED-required BUSY, CHECKOWNER). PROVED.
- Limits/warmup (api-cold.c): `valsize_max` hard cap `0x7FF00000` makes the
  `(int)` casts in `mdbx_env_get_maxvalsize_ex` etc. safe; warmup touch
  loops read-before-advance with `offset < used_range` invariant (used_range
  > 0 on any open env), timeout checked only inside the force-only block,
  `/dev/null` fd closed on all paths; `set_flags` envmode publish is a CAS
  loop with upgrade/downgrade pairing and unlock on every bailout. PROVED.
- Logging arithmetic (blanket per file): every monotime subtraction is
  guarded (`x > y` or `_noUnderflow`); txn_latency fields use guarded
  diffs; ratio2digits/percents validate buffer size and zero denominators.
  PROVED (all files).

## 2. Invariants to add to `docs/engineering/libmdbx-invariants.md` (S1 area)

**INV-API1 (release validation vs documented-UB boundary)**: every public
api-*.c entry point either fully validates its arguments in release builds
(null, signatures, dbi ranges, size/bytes gates — see §1.2 table) or belongs
to the explicitly documented UB family (`mdbx_cmp`/`mdbx_dcmp`; key2value
converters — pending the doc warning). No public entry point performs a
raw user-length memcpy (`memcpy_checklen` only, api-txn-data.c:464) and no
public entry point leaves a registered cursor dangling except V-037
(known).

**INV-API2 (option matrix symmetry)**: for every `MDBX_opt_*`, get returns
exactly the stored representation of the last accepted set (round-trip
identity within documented rounding), with two exceptions by design:
`sync_bytes` (F-V9-1 truncation — to be fixed) and txn_dp_limit (get
returns the post-adjustment value, not the raw argument). The non_auto
bits pin user-provided values against default recomputation exactly for
rp_augment_limit, dp_limit, prefault_write.

**INV-API3 (defrag/checkpoint txn ownership)**: an internally-started basal
txn in `mdbx_env_defrag` is ended on every exit path — either explicitly
(`txn_basal_end` at :228-231/:241-242), or because `txn_basal_checkpoint`
ends it internally on failure (txn-basal.c:525-531) which is why the
checkpoint-failure path nulls `txn` before the bailout label. `dfc` is
zeroed by `defrag_init` before any early return, making the bailout
`defrag_result`/`defrag_destroy` calls safe on partially-initialized state.

## 3. Test-coverage gaps to add to `docs/engineering/test-coverage.md`
(argument-validation fuzz surface)

| # | gap | where | priority |
|---|---|---|---|
| T-V9-1 | Option-matrix fuzz: for each `MDBX_opt_*`, set boundary values (`0`, `1`, sentinel, max+1, UINT64_MAX) then get and compare against the documented representation — catches F-V9-1 class regressions | api-opts.c | high |
| T-V9-2 | `sync_bytes` ≥ 2^(32+ps2ln) acceptance check (must EINVAL after the F-V9-1 fix; currently silently truncates) | api-opts.c:209 | high |
| T-V9-3 | key2value converters with wrong-length `MDBX_val` under ASAN (documents the assert-gated precondition; also motivates the doc warning) | api-key-transform.c | medium |
| T-V9-4 | `mdbx_dbi_dupsort_depthmask` against a crafted nested-tree height ≥ 32 (A7-violation build or chk-style hand-crafted page) — currently UB shift in release | api-txn-data.c:41 | medium |
| T-V9-5 | amend matrix: {TRY, no-TRY} × {parked, not-parked, finished, errored} — the TRY+PARKED leg is machine-checked today only by `repro/v9-txn-amend-park-try.c` | api-txn.c:747 | medium |
| T-V9-6 | `mdbx_env_info_ex` on a not-open env: assert bootid/sys fields zero + geo preserved (#171 contract sentence) | api-env.c:761-777 | low |
| T-V9-7 | `mdbx_gc_info` on an empty GC table: pin the MDBX_NOTFOUND (not SUCCESS) contract | api-gc.c:49-50 | low |
| T-V9-8 | checkpoint latency contract: no-dirty RESULT_TRUE path leaves `MDBX_commit_latency` zeroed (pin or fix together with F-V9-2) | api-txn.c:449 | low |
| T-V9-9 | `mdbx_env_set_option` spill denominators = 0 (zero-guard semantics at spill.c:179/183, txn-nested.c:470) | api-opts.c | low |

**R-V9-1 (refuted hypothesis, machine-checked)**: "amend(TRY) of a parked
reader re-parks and fails BAD_TXN" — false: the flag-value comparison
(`MDBX_TXN_TRY = 0x10000000 ≠ MDBX_TXN_PARKED`) skips re-parking;
`repro/v9-txn-amend-park-try.c` proves SUCCESS with a live write txn
(kept as regression test T-V9-5).

## 4. Findings register (V9)

| id | verdict | severity | summary |
|---|---|---|---|
| F-V9-1 | PROVED (machine) | low-medium | `MDBX_opt_sync_bytes` stores page count as uint32 without range check → silent wrap; `repro/v9-api-opts-syncbytes-truncation.c` |
| F-V9-2 | PROVED-A | low (doc) | `mdbx_txn_checkpoint` RESULT_TRUE no-dirty path skips `txn_latency_done` → zeroed latency on a success return |
| F-V9-3 | PROVED-A | low (doc) | not-open `env_info` branch wipes `mi_bootid.current`/`mi_sys_*` just filled by `env_info_sys` (#171 zeroed-rest semantics — document explicitly) |
| F-V9-4 | PROVED-A | low | `estimate()` computes `items * diff` in ptrdiff_t — wraps for ≥ ~2^31.5 entries with full-range diff (clamped output, approximation contract; A7-safe for crafted inputs) |
| F-V9-5 | PROVED-A | low (doc) | key2value `from_key` converters lack the explicit UB-warning sentence mdbx_cmp has; release reads up to 8 bytes of a short caller buffer |
| F-V9-6 | PROVED-A | low | `mdbx_cursor_scroll(INT64_MIN)` / `mdbx_is_readahead_reasonable(INT64_MIN redundancy)`: formal `-INT64_MIN` overflow (two's complement no-op in practice) |
| F-V9-7 | PROVED-A | low (A7) | `dupsort_depthmask` `1 << nested_height` on unvalidated height — UB shift under crafted DB (A7-hardening list) |
| R-V9-1 | REFUTED (hypothesis) | — | amend TRY+PARKED re-park — machine-refuted, repro kept as regression test |
