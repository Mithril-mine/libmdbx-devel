# Verification additions — stream D «Оптимизатор»: engine hot paths (performance audit)

Scope: performance-only review of the engine hot paths (reader ops, writer
commit pipeline, GC alloc/store, tree search, cursor movement, iov write
batching, comparators, public inline API). This is NOT a correctness pass —
the correctness lenses (A/B/C, waves 1-8) already covered these files; their
facts (LEM-V4-1 lower-bound search, LEM-V3-1/INV-P1 iov restamping,
INV-V4-Z, etc.) are cited, not re-verified. No behavior changes are proposed.

Method: full read of every listed target + machine checks:
- real-object disassembly (gcc 15.2 `-O2 -std=gnu11`, the tree's default
  build flags) of `search_branch_uint64` and `pnl_merge` from the actual
  `src/*.c` objects (not replicas);
- micro-probes in `docs/verification/repro/`:
  - `v-perf-branchless-search.c` — branchless search cycle vs classic 3-way
    vs sort.h `SEARCH_IMPL` cmov-bsearch, unique 1M-query stream, gcc+clang;
  - `v-perf-pnl-merge-cmov.c` — the gcc pointer-form merge (branchless,
    disassembly-verified) vs a textbook branchy merge over balanced
    random-interleaved / alternating / predictable inputs;
  - `v-perf-pnl-cut-shift.c` — the scalar tail-shift loops of
    `pnl_cut`/`gc_repnl_get_sequence`/`refund_reclaimed` vs `memmove`.
- host: Intel Xeon Gold 5317 (Ice Lake-SP), gcc 15.2.0, clang 21.1.8.
  perf counters unavailable (kernel.perf_event_paranoid=4), so mispredict
  counts are inferred from timing deltas only.

Finding row format: `P-id | file:line | frequency | cost | recommendation |
expected gain | risk`.

Known bottlenecks referenced, not duplicated: V-004 (gc dense-solver
exponential search), V-033 (hsr-decline loop), V-039 (cursor_distance
multi-page hive accounting). V-002 (NEON scan4seq) is correctness scope.

---

### 1. src/pnl.c + pnl.h + sort.h — PERF VERDICT (GAPS: 3)

- `P-1 | pnl.c:377-385 (pnl_cut), gc-get.c:759-768 (gc_repnl_get_sequence
  tail-shift), refund.c:25-27 (refund_reclaimed shift) | warm→hot (per
  multi-page GC allocation; per commit refund) | tail removal and span cut
  use one-element-per-iteration copy loops instead of memmove()`.
  Evidence (probe `v-perf-pnl-cut-shift.c`, Ice Lake-SP, gcc -O2;
  ratios stable across runs, absolute ns vary ~±40% run-to-run):
  len=100000 cut@1 span=2 → scalar ~31-63 µs vs memmove ~6.0-7.5 µs
  (5.2-8.5x); len=100000 cut@middle → ~15-31 µs vs ~3.0-3.7 µs (5.2-8.5x);
  len=2048 cut@1 → ~640-1270 ns vs ~77-120 ns (8.3-10.7x). Rationale: the
  scalar loop is load/store
  dependent (~1 elem/iteration, no vectorization possible through the
  loop-carried store chain), memmove moves 32-64 B/cycle.
  Frequency nuance: `gc_repnl_get_sequence` picks the tail-most sequence
  (scan4seq scans backward from `MDBX_PNL_EDGE`), so the shifted tail is
  often short; but sequences are repeatedly consumed from the tail, the
  tail accumulates non-contiguous leftovers, and matches migrate toward
  the middle/head → long-shift cases occur within every large commit.
  `pnl_cut`'s heavy callers (`pnl_get_best_sequence`,
  `pnl_crop_tail_sequence` at defrag.c:569/978/1030/1069) are cold but run
  over very large lists (the whole repnl). `refund_reclaimed` (refund.c)
  runs per commit whenever the repnl head abuts `first_unallocated` and
  shifts `len - k` elements scalar.
  Recommendation: hybrid removal — when `(len - pos) > ~32` elements use
  `memmove(pnl+pos, pnl+pos+span, (len-pos-span+1)*sizeof(pgno_t))`,
  otherwise keep the scalar loop (it wins below the memmove call overhead:
  ~2-4 ns vs ~4-6 ns at cut@tail). Purely mechanical; no ordering/size
  changes; the same shape covers all three sites.
  Expected gain: up to 8x on the shift portion; end-to-end ~1-6 µs saved
  per long-shift call at 100k-entry lists, i.e. a few % of GC-heavy
  commits, more for defrag/check workloads. Risk: LOW. Micro-benchmark
  guard: YES (`v-perf-pnl-cut-shift.c` already measures the two variants;
  wire it as a unit benchmark before/after).

- `P-2 | pnl.c:135-151 (pnl_insert_span) | warm (page-ops.c:657 dirty-page
  reclaim; txn-nested.c:84/399 nested absorb; loose refund) | fused
  scan+shift loop inserts high-pgno spans at the FRONT of the descending
  repnl, walking the whole list scalar`.
  The backward loop `while (r && DISORDERED(pnl[r], pgno)) pnl[w--] =
  pnl[r--]` is efficient only when the insertion point is near the tail.
  Spans produced by reclaim/refund carry the highest pgnos of the
  allocation frontier → insertion position is index 1 → O(len) scalar
  shift of the entire repnl (len up to rp_augment_limit).
  Recommendation: compute the insertion index with the existing cmov
  `pnl_search` (O(log n)), `memmove` the tail once, then run the existing
  fill loop; keep today's fused loop for short distances (same ~32-entry
  threshold as P-1). Expected gain: O(n) → O(log n + n/8) for
  front-insertions; ~30 µs → ~4 µs at 100k entries per reclaim/absorb
  event. Risk: LOW-MEDIUM (must preserve the no-duplicates precondition —
  asserted today via CHECKS2 in `pnl_append_prereserved` only; the fused
  loop's implicit `pnl[r] != pgno` stop relies on the same sortedness, so
  behavior is unchanged for valid inputs). Micro-benchmark guard: YES
  (extend the cut-shift probe with an insert-span case).

- `P-3 | pnl.c:191-223 (pnl_merge_inner) | warm-hot (one merge per GC
  record reclaimed; O(repnl) each) | the branchless cmov merge has a
  ~10-12 cycle serialized dependency chain per element; on the probe CPU a
  branchy merge is ~4.6x faster on fair-random interleaves`.
  Evidence: disassembly of the real `pnl_merge` (gcc 15) confirms the
  pointer-form compiles to `cmp/setb/cmova` + single back-edge — the
  branchless intent IS honored (the in-source per-compiler asm comments
  are accurate). Probe timings (balanced 2048+2048 collision-free
  descending lists, faithful dst/sentinel layout, warm L1, gcc -O2):
  branchy 0.67-0.68 vs branchless 3.06-3.10 ns/elem on fair-random
  interleaves; pattern isolation (predictable/alternating/random) shows
  the branchy variant wins in all three regimes (0.30/0.43/0.92 vs
  1.52/3.24/3.18 ns/elem). The
  dependency chain (cmov-updated pointers → next load address → compare →
  cmov) sets a hard floor the branchy loop avoids when its branches are
  even partially predictable; modern TAGE predictors render the original
  "mispredict-dominated" premise weaker than when the design was made.
  Counterweights (why this is NOT a definite change): (a) large merges are
  L2/L3-resident; mispredict penalties overlap memory latency, shrinking
  the gap; (b) gcc/clang codegen for the branchy form varies (the source
  comments document historic regressions); (c) the merge is load-bearing
  for the #83/#452 duplicate-flag protocol (correctness-stream fact).
  Recommendation: do not change by default; add a MPROBE-gated branchy
  variant and benchmark under real GC workloads (heavy recycling) before
  considering a swap; alternatively pipeline the branchless loop (load
  both candidates one iteration ahead) which keeps the branchless
  contract and breaks the dependency chain.
  Expected gain: up to ~4x on the merge loop itself per the probe;
  path-level gain unproven. Risk: MEDIUM-HIGH (data-dependent perf
  inversion). Micro-benchmark guard: YES (`v-perf-pnl-merge-cmov.c` is
  committed with a self-checking faithful harness; needs a realistic
  repnl/GC-record list generator before any decision).

Other pnl/sort observations (negative results, i.e. already optimal):
- `pnl_sort` radix threshold 142 with preallocated buffer
  (`MDBX_PNL_PREALLOC_FOR_RADIXSORT=1`): sound — radix does 2×(count+
  scatter)+memcpy passes for ≤32-bit keys with early-outs
  (`key_diff_mask<256`), beating N·log2(N) comparisons well below 142.
- `pnl_search`'s cmov bsearch (sort.h `SEARCH_IMPL`): measured FASTEST of
  all three variants at every size (n=16: 3.5 ns vs 6.1/6.6; n=8192: 13.7
  vs 33/60) — see P-4; optimal.
- `pnl_maxspan`/`pnl_scan_span`: branchless (`span += contiguous`,
  `scan += 1 - contiguous`), correct under stale-span no-match semantics
  (a wrong span never produces a false positive since CONTIGUOUS requires
  exact difference); O(n) with early exit only in
  `pnl_get_best_sequence`; called from `gc_reclaimed_maxspan_chunk` only
  under slot deficit — cold. Optimal.
- The 21/12 short-merge thresholds avoid the libc call for small merges;
  `pnl_merge`'s memcpy/memmove fast paths handle non-overlapping orderings
  before falling to the elementwise merge. Optimal.

### 2. src/dpl.c + dpl.h — PERF VERDICT (OPTIMAL)

- `P-6 | dpl.c:293-336 (txn_dpl_append) | hot (per dirty page) | no gap`.
  The pivot heuristic (items[length-42]) bounds in-place insertion work at
  42×16-byte shifts; the cmov pre-read of `pivot_pgno` removes a branch;
  the unsorted-tail shift uses a volatile 128-bit load/store on 64-bit
  gcc; the append-to-tail path is O(1). Amortization to
  `txn_dpl_sort_slowpath` (radix ≥142, else sorted-prefix quicksort +
  backward cmov merge using the reserved `dpl_gap_mergesort=16` gap) is
  the right cost model for commit-time workloads. The `dpl_insertion_threshold=42`
  constant shows no cache-behavior pathology (42×16B = 672B ≈ 10.5
  cachelines touched per worst-case in-place insert, all sequential).
- `txn_dpl_search`: ≤7-entry linear scan of the unsorted tail (jump-table
  switch, straight-line compares) + cmov bsearch of the sorted prefix —
  optimal for the lazy-sorted design.
- `txn_dpl_sift`: two-pointer merge removal, O(n+m), single pass — optimal.
- `txn_dpl_remove_ex`: memmove per removal — already vectorized; callers
  that remove many entries use the batch `txn_dpl_sift` instead. Optimal.

### 3. src/rkl.c — PERF VERDICT (OPTIMAL with nuances)

- `P-7 | rkl.c:242-392 (rkl_push) | warm (per reclaimed GC slot; per
  returned-id) | three nuances, none worth a change`:
  (a) backward shift insertion is O(n) per push worst-case, but the
  solid-interval design plus the swap heuristic (list_length > 16 &&
  adjacency) keeps the list short by construction;
  (b) `rkl_merge` (rkl.c:425-443) pushes elements one-by-one in descending
  order → each lands at the list front → O(n·m) for large disjoint sets;
  reachable only via `gc_search_holes`→sequel merge (rare per the source
  contract, gc-put.c:803-818) and `gc_update`'s ready4reuse merge; the
  solid-interval absorption caps practical sizes;
  (c) `list_remove_first` (rkl.c:183-195) shifts the list per pop with the
  acknowledged in-source TODO (headroom/two-cursor layout); pops happen
  per returned GC id — warm, short lists.
  The swap-vs-insert cost model (rkl.c:317-390) fires only on adjacency
  growth past 16 entries — its complexity is amortized noise; `const bool
  known_continuous = false` (rkl.c:245) is dead but compiles out.
- `rkl_contain`/`rkl_find`: solid-interval range check + cmov bsearch —
  optimal. `rkl_hole`/`rkl_turn`: O(1)-ish per step with cached
  solid_offset — optimal.

### 4. src/gc-get.c — PERF VERDICT (OPTIMAL; P-1 covers the cut loops)

- repnl_get_single: descending-list truncation-only removal (no shift) —
  optimal, as noted in the task.
- `P-14 | gc-get.c:102-553 (scan4seq family) | hot within GC allocation |
  optimal`. Runtime dispatch (SSE2/AVX2/AVX512BW/NEON + scalar fallback,
  `__builtin_cpu_supports` one-time resolver, one indirect call per scan
  amortized over the O(len) SIMD scan); backward scanning returns the
  tail-most match first which also minimizes the P-1 cut shift; page-edge
  masked loads avoid the 4-28 byte overread. Note (not a finding): the
  AVX-512 variant handles tail via AVX2/SSE2 downgrades — fine on Ice Lake
  (no downclock concern at these short bursts).
- gc_alloc_ex: syscall avoidance verified — `monotime_begin` is computed
  only when `num > 1 && gc_time_limit` (or PROFGC), `now_cache`
  expire_countdown=1 avoids repeated clock reads; the steady-sync decision
  block runs only on GC depletion. Optimal.
- page_alloc_finalize: mincore-gated prefault writes with iov batching
  (`MDBX_AUXILARY_IOV_MAX=64`, run-start coalescing of consecutive
  non-incore pages, single flush per 64) — the stub writes go through
  `osal_dxb_write_stubv` (pwritev) — optimal as designed; the
  `env_is_page_incore` cache is O(1) on the hot 64-page unit.
- `P-13 | gc-put.c:550-564 (gc_dense_hist) | cold (rarity verified) |
  optimal`. O(repnl) histogram runs only from `gc_handle_dense`, reachable
  only when `dense_unfit > 0` (gc-put.c:1229) — the documented
  legacy-huge-GC-record condition; V-004 already registers the solver
  liveness issue on the same rare path.

### 5. src/gc-put.c — PERF VERDICT (OPTIMAL)

- gc_store_retired (BIGFOOT): chunking into `goodchunk` records is
  amortized by design (per-record cursor_put + memcpy; stockpile reserves
  batched via `gc_prepare_stockpile4retired`); `chunk_hi` alignment `|3`
  avoids degenerate 1-2 page records. Per-commit cost is
  O(retired/goodchunk) tree ops — minimal for the format.
- gc_rerere / gc_reserve4return: the common path (`left_min <
  maxgc_large1page && ready4reuse non-empty`) is one rkl_pop + rkl_push +
  cursor_put; chunk_lo/chunk_hi bracketing amortizes repnl growth during
  reservation. Reservation is re-done only on MDBX_RESULT_TRUE restarts
  (loop-bounded by `ctx->loop > 42`). Optimal.
- gc_fill_returned: single-slot fast path (the majority case) is
  `gc_enforce_not_spilled` (one cursor_seek) + memcpy; the surplus
  distribution uses fixed-point `(chunk_hi*factor)>>32` with no division
  in the loop. Optimal.

### 6. src/tree-search.c — PERF VERDICT (OPTIMAL; one re-benchmark note)

- `P-4 | tree-search.c:158-167 + 539-566 | hot (per level of every
  search) | the branchless cycles compile branchless — verified on the
  real object`. Disassembly of `search_branch_uint64` (gcc 15 -O2):
  `cmp; je <exact-exit>; sbb %rax,%rax; and; and; add; add; jne <loop>`
  — the descent is mask-based (sbb sign trick, no cmov needed), the only
  branches are the predicted-not-taken exact-match exit and the
  loop-exit. The macro structure has no hidden data branches; the
  `CLEAR_VALUE_PROPAGATION` barrier does its job (the unbarriered replica
  compiles measurably faster, i.e. the barrier is what keeps gcc from
  turning the mask chain into branches). The LEM-V4-1 lower-bound shape
  is preserved in codegen.
- Per-level indirect dispatch through `mc->clc->k.search_branch`: one
  predicted indirect call per level; the resolved function is cached in
  the shared per-env `kvx` clc entry on first search
  (tree_search_branch_configure) — amortized to zero. Specializing the
  whole descent per comparator would explode code size for ~2-5 cycles
  per level; not worth it. Optimal.
- Re-benchmark note (not a gap): probe timings (orderings stable across
  runs, absolute ns vary ±40% run-to-run) show the sort.h-style cmov
  bsearch (no early exit, 2-element epilogue) is the fastest variant at
  ALL sizes (n=16: ~3.5-6.5 ns vs ~6-11; n=8192: ~13.5-26 vs ~33-46/60-79),
  while the early-exit branchless cycle and the classic branchy 3-way
  search are within noise of each other at branch-fanout sizes, with the
  branchy variant tending to win below ~1-2k keys and losing badly at ≥8k
  (mispredict-dominated). Caveat: the probe's unique-query stream still
  lets the predictor partially learn key-correlated patterns, understating
  the branchy variant's mispredict cost; real lookups are heavily skewed,
  favoring it further. Conclusion: current design is sound; revisit only
  if profiling on target hardware shows search-bound workloads.

### 7. src/cursor-nav.c + cursor-seek.c — PERF VERDICT (OPTIMAL)

- `P-15 | cursor-nav.c:347-421 (cursor_forward_at/backward_at) | warm |
  page-by-page sibling walk is format-optimal`. O(amount/pagesize)·height;
  jumping whole subtrees in O(height) would require per-subtree item
  counts, which branch nodes do not store (no room in the on-disk format
  without a layout change — out of scope for a performance pass).
  Everything else in the pair is O(1) per op: `cursor_step`'s common path
  (state==0, no NODUP, not inner) is flags-load + ki/nkeys + one compare +
  `cursor_bring` (check_leaf_type + node_read); the state-machine flags
  (`z_after_delete/z_hollow/z_eof_*`) are bitmask tests, not switches.
- `sibling()` recursion depth is bounded by tree height; `cursor_enroot`
  round-trip costs one flags AND + OR. Negligible.
- cursor-seek.c `cursor_seek` fast path: first/last-node comparisons
  before the full `tree_search` (3 compares hit the most common
  forward-scan patterns); the ki[0] mid-page check avoids the descent
  entirely for same-page seeks — exactly the hot case for
  point-lookups-after-previous. Optimal. (V-039 distance accounting and
  V-035 wrap belong to correctness/registered-gap scope.)

### 8. src/txn-basal.c — PERF VERDICT (OPTIMAL)

- `P-5 | txn-basal.c:50-53 | per commit | verified non-issue`: the
  `while (r <= dl->length) dl->items[++w] = dl->items[r++]` tail
  compaction is a NO-OP on the success path (r == length+1 after the
  main loop) and compacts only on mid-commit iov_write failure — the
  task's "dirtylist tail compaction O(n) per commit" concern is absent by
  construction.
- txn_write: single sorted pass over the DPL (sorted by
  `txn_dpl_sort`), per-page `iov_page` with `is_shadowed` re-stamp per
  INV-P1 (malloc mode), loose-page compaction fused into the same pass —
  one pass total. Optimal.
- txn_basal_commit meta image assembly: field-wise copy (~8 stores +
  2 tree_t copies + bcopy_16 + unaligned poke) instead of a struct copy —
  minimal; the pure-transaction fast return avoids meta write entirely.
  The nometasync prev-meta flush decision is a single compare chain. Optimal.

### 9. src/mvcc-readers.c — PERF VERDICT (OPTIMAL)

- `P-8 | mvcc-readers.c:132-184 (mvcc_snapshot_oldest_rw) | per commit |
  optimal`. Cost model already in place: (a) refresh-flag double-check;
  (b) early break at `oldest_txnid == prev_oldest` (MDBX_CHECKING<1) —
  readers below the previous oldest are kicked/reset rather than counted,
  so the break is semantically safe (stream-B facts); (c) 4-pass bound
  with `may_lag` (used by txn.c:41 detent refresh) returns prev_oldest
  under churn; (d) per-reader work is 4 relaxed/acquire loads with a
  retry on torn (retired,txnid) pairs. Steady-state commit cost is one
  scan of the reader table; the kick path is V-033 (reference only).
- mvcc_bind_slot publish ordering: correctness-motivated (INV-S9-1);
  the cost is one mutex-protected loop with a free-slot scan — O(n) per
  reader start, off the commit path. Optimal.
- mvcc_cleanup_dead: __cold, pid-dedup via binary-insert with O(n) shifts
  — irrelevant at call frequency (reader-full / kick / rthc).

### 10. src/page-iov.c + osal/posix/osal-ioring.c — PERF VERDICT (OPTIMAL)

- `P-10 | osal-ioring.c:51-108,129-158 | per commit | the POSIX ioring is
  a pwrite/pwritev stub by design (io_uring left as in-source TODO)`. The
  batching that matters is present: adjacent-offset items merge into one
  item; when the data buffers are ALSO contiguous the last iov is extended
  in place (single pwrite of the whole run); items inherit the DPL's pgno
  order so no resort is needed. Scattered sets cost one pwritev syscall
  per run — the known/accepted design point; real io_uring would change
  the syscall shape, not this code's complexity. `iov_callback4dirtypages`
  release walk: single-page fast path + chunk walk for large pages;
  coherency memcmp is gated off unless the incoherence counter is set
  (the documented 10-15% issue). Optimal for the stub design.

### 11. src/comparators.c/h + node.h — PERF VERDICT (OPTIMAL)

- `P-11 | comparators.h / node.h | hot (every compare, every node touch) |
  no gaps`. On x86-64 `MDBX_UNALIGNED_OK=8` makes all UNALIGNED_PEEK
  accessors single loads (no memcpy-expand); `cmp_lexical` = libc memcmp
  on the shortest prefix + length difference (glibc memcmp is AVX2/AVX512
  vectorized); `cmp_reverse` loads words backward from the key tails with
  a 3-byte tail trick — optimal given keys are compared high-byte-first
  in reverse order; `cmp_lenfast` compares length first (single branch
  for the overwhelmingly different-length case). The align4/align2 vs
  unaligned variants are selected at dispatch time (`cursor_to_search_*`)
  — no per-compare cost. `get_key` reads ksize+flags (2 loads) per probe;
  dupfix pages skip node headers entirely (`page_dupfix_ptr`) — already
  the minimum possible.

### 12. mdbx.h inline API — PERF VERDICT (OPTIMAL)

- `P-12 | mdbx.h LIBMDBX_INLINE_API (29 functions) | per call | no
  argument-check overhead added by the inline layer`. The wrappers are
  thin delegators to the exported `_ex` functions; `MDBX_INLINE_API_ASSERT`
  compiles to nothing when MDBX_CHECKING/MDBX_DEBUG are off (mdbx.h:646-656);
  checks like `check_env/check_txn` live in the callee exactly once.
  `likely()/unlikely()` placement is consistent through the internal
  check chain (utils.h MDBX_LIKELY on the success conditions). No finding.

---

## Summary table

| id | file:line | freq | cost | recommendation | gain | risk |
|----|-----------|------|------|----------------|------|------|
| P-1 | pnl.c:377, gc-get.c:759, refund.c:25 | warm→hot | scalar tail-shift, 5-8x slower than memmove (measured) | hybrid memmove when tail > ~32 elems | up to 8x on shift; µs-scale per long cut | LOW |
| P-2 | pnl.c:135 (pnl_insert_span) | warm | front-insertion walks whole repnl scalar | bsearch + memmove + fill, short-case fallback | O(n)→O(log n + n/8) per front-insert | LOW-MED |
| P-3 | pnl.c:191 (pnl_merge_inner) | warm-hot | branchless merge ~10-12 c/elem dependency floor; branchy ~4.6x faster on warm random data (probe) | benchmark-only first; optionally software-pipelined branchless | up to ~4x merge loop, path-level unproven | MED-HIGH |
| P-4 | tree-search.c:158-167 | hot | none — branchless codegen VERIFIED (sbb/ands + jne) | none; note: cmov-lb variant fastest at all sizes in probe | — (negative result) | — |
| P-5 | txn-basal.c:50-53 | per commit | none — tail compaction is error-path only | none (verified non-issue) | — | — |
| P-6 | dpl.c:293-336 | hot | none — 42-window bounded | none | — | — |
| P-7 | rkl.c:425-443,183-195 | warm | rkl_merge push-by-push O(n·m), remove_first O(n); solid-interval keeps lists short | none (accepted design; TODO acknowledged in source) | — | — |
| P-8 | mvcc-readers.c:132-184 | per commit | none — early-break + 4-pass bound + 4 loads/reader | none | — | — |
| P-10 | osal-ioring.c | per commit | none for a pwritev stub; io_uring is a documented TODO | none (format/syscall-shape decision) | — | — |
| P-11 | comparators.h/node.h | hot | none — memcmp-based, single-load peeks | none | — | — |
| P-12 | mdbx.h inline API | per call | none — thin delegators | none | — | — |
| P-13 | gc-put.c:550 (gc_dense_hist) | cold | none — rarity condition verified (dense_unfit>0) | none | — | — |
| P-14 | gc-get.c scan4seq | hot-in-GC | none — runtime-dispatch SIMD, tail-first scan | none | — | — |
| P-15 | cursor-nav.c:347-421 | warm | none — page-walk is format-optimal (no subtree counts in branch nodes) | none | — | — |

## Established performance invariants (negative results worth keeping)

1. INV-PERF-1 (search codegen): the BINARY_BRANCHLESS_SEARCH_CYCLE macros
   compile branchless on gcc 15 x86-64 — mask-descent (sbb) + single
   loop-exit branch + predicted-not-taken exact-exit; the
   CLEAR_VALUE_PROPAGATION barrier is load-bearing for keeping the mask
   chain opaque (removing it measurably degrades gcc codegen).
2. INV-PERF-2 (sort.h cmov bsearch): `SEARCH_IMPL`'s no-early-exit cmov
   bsearch is the fastest variant at every probed size (n=16..8192, both
   gcc and clang) — keep for pnl_search/rkl/dpl.
3. INV-PERF-3 (DPL pivot): the 42-entry in-place-insertion window bounds
   per-append work at ~42×16B shifts and defers re-sorting to the
   gap-mergesort slowpath; no cache pathology at this window size.
4. INV-PERF-4 (commit tail compaction): txn_write's dirtylist compaction
   is error-path-only; the success path performs exactly one sorted pass.
5. INV-PERF-5 (oldest scan): per-commit cost is one reader-table scan
   with early break at prev_oldest and a 4-pass churn bound; the
   stuck-reader kick is correctness-motivated (stream-B INV-S9 facts).
6. INV-PERF-6 (format-limited estimate): cursor_forward_at/backward_at
   page-walks because branch nodes carry no subtree item counts —
   an O(height) jump would require a layout change, not a code change.
7. INV-PERF-7 (merge codegen): the gcc pointer-form of pnl_merge_inner
   compiles branchless (cmov + setcc + single back-edge); the in-source
   per-compiler comments match observed codegen. The OPEN question is not
   codegen but algorithm choice on 2020s predictors (P-3) — measurable
   only with real-workload benchmarks.

## Probes

- docs/verification/repro/v-perf-pnl-cut-shift.c — scalar shift vs
  memmove (P-1/P-2 evidence).
- docs/verification/repro/v-perf-branchless-search.c — branchless cycle
  vs classic vs cmov-lb (P-4 evidence; gcc+clang).
- docs/verification/repro/v-perf-pnl-merge-cmov.c — merge variants over
  random/alternating/predictable interleaves (P-3 evidence).
