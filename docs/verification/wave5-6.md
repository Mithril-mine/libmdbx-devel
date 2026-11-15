# Waves 5–6 merged report + stream D «Оптимизатор» (performance)

Branch `verification/master-audit`. This merge covers seven agents launched
in parallel: V9 API surface (A), V10 foundation/defrag/chk (B, adversarial),
V11 OSAL posix+windows (C), V12 tools (A), V13a C++ core mdbx.c++ (B),
V13b C++ headers mdbx++ (C), and two stream-D performance auditors (engine
hot paths; API/C++/tools). Full per-stream ledgers: doc-additions-v9.md,
doc-additions-v10-b.md, doc-additions-v11c.md, doc-additions-v12.md,
doc-additions-v13a.md, doc-additions-v13b.md, doc-additions-perf-engine.md,
doc-additions-perf-api.md. Repros: v9-api-opts-syncbytes-truncation.c,
v9-txn-amend-park-try.c, v10b-defrag-{cycles,timelimit,adopt-cursor}.c,
v11c-osal-primitives-posix.c, v12-load-nul-swallow.c, v13a-*.cpp ×2,
v13b-*.cpp ×2, v-perf-*.c/cpp ×4.

Canonical registry continues from V-054: **V-055..V-068** below; performance
findings carry their own **P-** series (two per-stream series preserved).

---

## V9 API surface (stream A) — 8,132 lines, all 14 api-*.c

Verdict census: PROVED/PROVED·A throughout except the findings below. The
24-case `MDBX_opt_*` set/get matrix fully verified; the release
"assert-gate" inventory compiled (which public APIs validate only under
CHECKING — documented-UB boundary INV-API1); txn checkpoint/commit_embark/
amend ladders machine-checked (R-V9-1 hypothesis REFUTED — amend(TRY) of a
parked reader is correct, regression repro kept).

### Findings
| V-id | where | severity | essence |
|---|---|---|---|
| **V-055** | api-opts.c:209-228 | low-med, **machine-proved** | `MDBX_opt_sync_bytes`: byte bound only, then `bytes2pgno` stored as `(uint32_t)` without a page-count check → value ≥ 2^(32+ps2ln) silently wraps (set 2^44 → threshold 0). Repro included. Fix: EINVAL after bytes2pgno |
| V-056 | api-txn.c:449-450 | doc | `mdbx_txn_checkpoint` RESULT_TRUE no-dirty path skips `txn_latency_done` → zeroed latency on success |
| V-057 | api-env.c:761-777 | doc | not-open `env_info` branch wipes mi_bootid/mi_sys filled by env_info_sys — document the "zeroed rest" contract |
| V-058 | api-range-estimate.c:96 | low | `items * diff` ptrdiff wrap at ≥ ~2^31.5 entries × full-range diff (approximation contract; clamped output cannot repair) |
| V-059 | api-key-transform.c:142-160 | doc | from_key converters: ASSERT-only length checks — recommend the mdbx_cmp-style explicit UB-warning sentence |
| V-060 | api-cursor.c:1077; api-misc.c:17 | formal | `−INT64_MIN` UB (two's-complement no-op in practice) |
| V-061 | api-txn-data.c:41 | low/A7-hardening | `mdbx_dbi_dupsort_depthmask`: `1 << raw nested-tree height` bypasses tbl_fetch's release height check → crafted height ≥ 32 = UB shift |

New lemmas: LEM-V9-CACHE-SEQLOCK (MT cache publish is a sound seqlock —
poison precedes field writes, publish restricted to CONFIRMED/REFRESHED),
LEM-V9-FALLBACK-DEPTH (=1), LEM-V9-BUNCH-PAIRING, LEM-V9-DEFRAG-OWNERSHIP,
LEM-V9-AMEND-NO-LOCK-LEAK, LEM-V9-VSIZE-CAP (valsize_max ≤ 0x7FF00000 makes
the int getters and cache uint32 length safe for validated values),
LEM-V9-NULL-NAME.

## V10 foundation/defrag/chk (stream B, adversarial) — no corruption/UB defect; 3 machine-checked contract findings

All ten attack areas held: defrag #306 gate correct (adoption by design;
the fix present at defrag.c:474-477); walk_stack[32] enforced by a plain
release `if` → CORRUPTED; **arcs-map cycle safety is the alternation of
walk options** (cycle-2+ walks with dont_walk_MAIN; named tables reachable
only through MAIN) — new **INV-D1**; remapped/retreat edges monotone; chk
#360 problem/decline ledger counted exactly once — **INV-C2**; chk-walk
clamped-GC-record containment verified end-to-end incl. the unclamped
pnl_scan_span bound — **INV-C3**; safe64 poison-before-publish/±1 ABA
guards; atomics mo_* mapping has no downgrade and the non-C11 gate is
total; ENSURE→panic_func then unconditional osal_panic; options sweep found
no un-enforced default; cogs ps=256 bounds hold; comparators have no
misaligned reach (check_key bcopy into intbuf + the V-045 pin).

### Findings (machine-checked, all safe-direction)
| V-id | where | severity | essence |
|---|---|---|---|
| V-062 | api-gc.c:166-171 (defrag) | doc/contract minor | stuck-retry stop returns rc=SUCCESS + `noobstacles(0)` despite a named obstacle ("bailout since stucked", pages_left hint only). Repro v10b-defrag-cycles.c |
| **V-063** | defrag.c:86-93 vs mdbx.h | doc/contract moderate, **machine-checked** | `time_limit` is evaluated only when `defrag_atleast` goal edge is reached; with an unreachable goal a 15 µs limit was ignored through 25 cycles (~3 ms). mdbx.h documents the cap as unconditional — REFUTED vs docs. Repro v10b-defrag-timelimit.c |
| V-064 | defrag adopted-txn | doc/contract minor, **machine-checked** | per-cycle `txn_basal_checkpoint` **commits the user's pending writes mid-txn** (pending put survived mdbx_txn_abort after defrag) and detaches user cursors (next get safely EINVAL — no UAF). Undocumented. Repro v10b-defrag-adopt-cursor.c |

Residuals: defrag score may decline across a productive cycle (heuristic,
review note); `pnl_get_best_sequence` detent underflow at the two defrag
call sites is self-healing (INV-D2: targets ⊆ repnl free pages — cannot
overlap a live source).

## V11 OSAL posix+windows (stream C) — all POSIX CONSISTENT; Windows CONSISTENT·A(win-reading)

pwrite/pread retry loops, fsetsize #444-era guards (incl. the 5.10-ext4
window and tmpfs-ENOSPC workaround), mresize mremap→append→unmap/remap
ladder with full old-mapping restore, mmap/munmap union nulling
(re-corroborates R-C3), bootid crafted-content resistance (machine-checked
probe: E1 fseek-shut/converge, E2 ioring state machine, E3 bootid), monotime
16.16 + the 0.1 s coherency constant, fsync mode decode — all verified.
Windows: overlapped IO, flush modes, #449 ior encoding/drain at all 5
decoder sites, suspend/resume statement-level completion.

### Findings
| V-id | where | severity | essence |
|---|---|---|---|
| **V-065** | osal/windows/osal-mmap.c (+207-210 fast path) | **major-if-confirmed (win-reading)** | native-Windows RDONLY mapping extent is fixed at open-time file size (NtCreateSection MaximumSize), while POSIX maps the full limit — a reader whose new snapshot needs pages beyond the extent hits osal_mresize without MAY_UNMAP → MDBX_EPERM → mdbx_txn_begin fails. Sub-item: the RDONLY fast path (map->current = size without section extension) is unreachable via the engine and unsound if reached. Needs Windows runtime confirmation; fix shapes proposed |
| V-066 | osal windows minors | low/info | ior drain give-up residual; stale-APC window (one alertable SleepEx hardening); MAY_SHRINK not honored on windows; MADV_DONTFORK error-path map leak; dead `boundary` field; <Vista ENOSYS; SYNC_SIZE-only Linux no-op (dead via engine decode); getenv "" divergence |

New invariants: INV-OS1 (ior slot accounting — merge costs 0 slots),
INV-OS2 (windows item encoding `(iov_len−1)&1` at all 5 decoder sites),
INV-OS3 (batch flush/drain contract), INV-OS4 (fd parking lot), INV-OS5
(mresize flag decode cross-platform), INV-OS6 (windows RDONLY extent
asymmetry = V-065).

## V12 tools (stream A) — 2,880 lines; no memory-safety violation: 600 mutated dumps × 6 tools × ASAN/UBSAN clean

### Findings
| V-id | where | severity | essence |
|---|---|---|---|
| **V-067** | load.c readline/readhdr | P3, **machine-proved** | NUL byte silently truncates a parsed line: ` \0ZZ…` + ` 353535` → phantom record ("","555"), tail dropped, exit 0. Repro v12-load-nul-swallow.c |
| **V-068** | load.c:787-797 | P2, **machine-traced** | `-p` purge fails MDBX_INCOMPATIBLE at the trailing MAIN section of every multi-table dump → partial load + exit 1 |
| V-069 | stat.c:310-334/432 | P7 | `-r`/`-rr` exit FAILURE on "reader table absent"/"now empty" (RESULT_TRUE −1 propagates to exit code) |
| V-070 | drop.c:149-158 | P3 | abort on a commit-terminated handle (contract violation; memory-safe today via basal retention — load.c shows the correct pattern) |
| V-071 | chk.c:172; common.h:146; load.c:431-457 | P4 | logger `fmt==nullptr` unguarded (-Wformat-overflow at -O1); `isxdigit(plain char)` UB on high bytes (dump.c does it right) |
| V-072 | copy.c:65-88; copy/drop signals; chk exit map; load lineno | P3-P7 | hand-rolled options (no clustering/`--`/unknown-option silence); SIGINT/SIGTERM installed but never polled during copy; EXIT_INTERRUPTED(5) unreachable on signals; EOF-in-value mislabeled; help detection scans past positionals; empty-MAIN drop refused with named tables — undocumented |

Positive anchors re-verified: #70 (dump ferror ⇒ exit 1 on /dev/full; load
truncation ⇒ ENODATA), #385 (byte-identical `-a` round trips; trailing-MAIN
partition exact), #312 (geometry/pagesize/sequence validation). New:
INV-T1 (dump-a partition), INV-T2 (concise grammar), LEM-V12-hex,
LEM-V12-readline-bounds, LEM-V12-maxdbs2, LEM-V12-commit-contract.

## V13a C++ core (stream B) — no memory-safety/lifetime/use-after-finish/leak defect anywhere in mdbx.c++

Managed-handle matrices verified per-exit (**INV-CXX1**: null-on-death /
keep-on-mismatch for env::close, txn commit/abort/checkpoint/embark/amend);
ctor exception safety by delegation (**INV-CXX2**, 50 failed opens LSan-
clean); user exceptions cross the C boundary only via exception_thunk
(**INV-CXX3**); V-037's C++ counterpart closed: a cursor_managed outliving
its txn is a safe detached cursor (machine-checked); transcoder bounds
canary 0..200 × 7 wrap widths; 21105 transcoder runs + 60k adversarial
decoder-agreement + ~80k is_printable cases pass.

### Findings
| V-id | where | severity | essence |
|---|---|---|---|
| V-073 | src/mdbx.c++:722-743/1132-1159 | low | `from_hex`/`from_base64` `is_erroneous()` reject zero-decodable-char sources that decode accepts → `is_hex("")==false`, `is_base58("")==true` — inconsistent empty-source predicate (the `got` flag) |
| V-074 | src/mdbx.c++ (is_printable) | low | ASCII-mode `slice::is_printable` accepts 0xE0-0xEF: `3<<LS`(48) aliases `P_`(16) in the mask — exhaustive-checked; sibling lead classes correct |
| V-075 | mdbx.c++ (checkpoint) | doc hazard | #92 commit message claims "Like checkpoint() already does…" — false; the HAS_CHILD guard lives in the C core's check_txn (behavior correct, machine-checked; the "checkpoint frees nested child" hypothesis REFUTED by control experiment) |

## V13b C++ headers (stream C) — REFUTED: none; SUSPECT: none

All 9 headers statement-level: slice safe_middle (#28) overflow arithmetic
complete; buffer modality state machine pointer-derived, move/reshape
transitions complete; scoped-enum breaking change keeps all 8 deprecated
aliases; put/replace flag forwarding exact; rc→exception mapping total;
ODR sweep clean; template matrix {char,wchar_t}×{4,8}×reference/allocated
+ runtime-CRUD repro pass.

### Findings
| V-id | where | severity | essence |
|---|---|---|---|
| **V-076** | mdbx.h:6331 + api-cold.c | low (ABI) | `mdbx_txn_release_all_cursors` (2-arg legacy inline) is the only one of 27 inline APIs **without an exported fallback** (was a real export until a3265e11). Fix = one line in api-cold.c |
| V-077 | mdbx++ minors | info | b58 size estimate 32-bit overflow for ~2 GB values (unreachable); out-of-range scoped-enum values from flag combos in map_handle::info; slice::assign stores base before the length check; buffer_pair_spec noexcept trait mismatch (pathological allocators); `is_hex("")`/`try_*_reserve` doc notes; non-template txn::replace leaves old_value untouched on dirty-page old values |

---

## Stream D «Оптимизатор» — performance (two auditors, machine evidence)

### Engine (P-engine series)
| id | file:line | freq | cost | recommendation | gain | risk |
|---|---|---|---|---|---|---|
| **P-E1** | pnl.c:377, gc-get.c:759, refund.c:25 | warm→hot | scalar tail-shift loops 5-10× slower than memmove (probe: len=100k → 31-63 µs vs 6-7.5 µs; scan4seq's tail-first pick fragments the tail and pushes matches mid-list → long shifts in large commits) | hybrid memmove when tail > ~32 elems | up to 10× on the shift | LOW |
| **P-E2** | pnl.c:135 pnl_insert_span | warm | front-insertions walk the whole repnl scalar (reclaim/refund spans carry highest pgnos → index-1 insertion) | bsearch + memmove + fill, short-case fallback | O(n)→O(log n+n/8) | LOW-MED |
| **P-E3** | pnl.c:191 pnl_merge_inner | warm-hot | branchless cmov chain floors at ~10-12 c/elem; branchy ~4.6× faster on fair-random warm data (probe 0.67 vs 3.06 ns/elem) — codegen intent IS honored (disasm verified); decision is algorithmic, not codegen | benchmark-gated under real GC workloads first; or software-pipelined branchless | up to ~4× merge loop, path unproven (cache-bound large merges shrink the gap; #83/#452 flag is load-bearing) | MED-HIGH |

**Valuable negative results (OPTIMAL, verified):** tree_search branchless
cycles DO compile branchless (real disasm: sbb-mask descent; the
CLEAR_VALUE_PROPAGATION barrier is load-bearing) — P-E4; sort.h cmov bsearch
fastest at all probed sizes 16..8192 (gcc+clang); DPL 42-window pivot,
txn_write compaction (error-path-only), oldest-scan early-break+4-pass bound,
cursor page-walks (format-limited), scan4seq dispatch, prefault batching,
dense-hist rarity, rkl cost model, comparators (UNALIGNED_OK single loads,
libc SIMD memcmp), mdbx.h inline layer — all verified optimal (INV-PERF-1..7).

### API/C++/tools (P-api series)
| id | file:line | freq | cost | recommendation | gain | risk |
|---|---|---|---|---|---|---|
| **P-A1** | tools/dump.c:40-57 dumpval | per dumped byte (cold) | locked putchar() per byte (2/byte in bytevalue mode) | putc_unlocked shim / line-buffered fwrite | 2-4× on the encode loop | none |
| **P-A2** | impl-cursor.h++:154-182 get_batch | per batch (hot bulk scans) | 2 heap allocs + extra 32B/pair staging copy | docs-first: point bulk users at C mdbx_cursor_get_batch; optional staging span | ~5-15% small batches | low (API shape) |
| **P-A3** | mdbx.h:5838 SingleThreaded doc | doc-driven | ambiguity pushes MT users onto the seqlock wrapper (+2 loads/+copy/+4 compares) | clarify per-thread entries are safe with mdbx_cache_get_SingleThreaded | few ns + removes a wrong reading | none |

**Valuable negative results:** per-call CRUD tax O(1) allocation-free
(INV-PERF-API1); cache HIT lockless, stable-DB CONFIRMED O(1) without page
access, confirm-walk worst case bounded by full search (INV-PERF-CACHE1);
**C++ zero-overhead machine-verified** — txn::get = one mdbx_get@PLT +
cold throw; txn::get_cached = pure tail jmp; slice derives MDBX_val (no
hidden copies) (INV-PERF-CXX1); copy pipelines use kernel fast paths + 1 MiB
batches (INV-PERF-COPY1).

---

## Cross-stream checks (waves 5-6)

- V9's LEM-V9-DEFRAG-OWNERSHIP ↔ V10's #306 attack: consistent (adoption by
  design, mid-commit semantics now documented as V-064).
- A's V9 defrag gate review and B's machine repros agree on the same call
  sites (api-gc.c:125-171).
- Tools V-067 NUL swallow vs engine's parser trust model A7: contained in
  the tool, no engine impact.
- V13a/V13b independently confirmed the exception-thunk boundary and ODR
  machinery from both sides (mdbx.c++ and the headers) — no contradiction.
- Perf stream D vs correctness streams: P-E1/P-E2 sites (pnl_cut/
  gc_repnl_get_sequence/pnl_insert_span) carry wave-1 correctness verdicts —
  the recommendations are shape-preserving; P-E3 explicitly does NOT change
  code without a workload benchmark (the #83/#452 duplicate flag is
  load-bearing — wave-1 PAIR-N1).

## Metrics after waves 5-6 + stream D

- Functions/areas with verdicts: ≈440 (w4) + ~150 (V9) + ~60 (V10) + ~40 (V11) + ~70 (V12) + 106 (V13a) + ~250 inline (V13b) + 12+8 perf targets ≈ **1,100+**
- Conditions/obligations verified: ≈4,400 + ~2,600 ≈ **~7,000 / 9,711** (remaining: A-lens statement-level top-up for gc-get/gc-put/txn files already covered adversarially + pairs; est. ~2,700 conditions, mostly overlap)
- Cumulative REFUTED defects: 10 (w1-4) + machine-proved V-055, V-063, V-067, V-068 + confirmed-contract V-060/V-062/V-064 + win-reading V-065 (major-if-confirmed) + minor families V-056..V-061, V-066, V-069..V-077 ≈ **40+ findings total, 16 machine-checked**
- Performance: 6 actionable optimizations (2 hot-path LOW-risk: P-E1, P-A1), 2 benchmark-gated decisions (P-E3, P-A2), 1 docs-only (P-A3); 15+ verified-optimal negative results.
