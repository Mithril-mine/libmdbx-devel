# doc-additions-perf-api — stream D «Оптимизатор» (performance audit: API layer, C++ wrappers, tools)

Companion to the correctness waves (`doc-additions-v2..v4c.md`). This tranche
is **not** a correctness pass: it audits where the API surface loses
performance, verifies (rather than assumes) the zero-overhead claims of the
C++ layer, and records the load-bearing performance invariants of the
reader-side get-cached feature. Machine evidence: `repro/v-perf-d-cxx-inline.cpp`
(compiled `-O2 -std=c++20`, gcc 15.2, x86-64; per-function assembly quoted
inline below). Frequencies: **per-call** = every API invocation on a hot path;
**per-txn** = once per transaction; **cold** = tools/setup/admin.

Reuses established facts: the one-compare cursor gate and PNL-slack memcpy
fast paths (`repro/v-perf-branchless-search.c`, `repro/v-perf-pnl-*.c`), and
INV-CACHE1 (cache-entry offset unenforced — hardening, no per-call cost).

---

## 1. Per-target verdicts

### 1.1 mdbx.h inline layer — PERF VERDICT (OPTIMAL)

- `LIBMDBX_INLINE_API` (mdbx.h:441-446) expands to `static __inline` wrappers;
  in library builds it additionally emits the ODR-common `__inline_<name>` impl
  plus an exported prototype. There is **no runtime dispatch** — the "dispatch"
  is plain static inlining.
- `MDBX_INLINE_API_ASSERT` (mdbx.h:646-655) compiles to `do {} while(0)` in
  release builds (only active under `MDBX_CHECKING`/`MDBX_DEBUG`), so the
  convenience shims (`mdbx_env_close`→`_ex`, `mdbx_txn_begin`→`_ex`,
  `mdbx_txn_commit`→`_ex`, …) are literally one tail-call.
- Data-path CRUD (`mdbx_get/put/del/replace/cursor_get`, mdbx.h:5617-6449) is
  deliberately **out-of-line** `LIBMDBX_API` — argument-check overhead lives
  once inside the library (see 1.2), not duplicated per user TU.
- Const helpers (`mdbx_key_from_int64/int32`, `mdbx_limits_*`) are
  `MDBX_NOTHROW_CONST_FUNCTION` one-liners.
- No MDBX_val helper in mdbx.h copies; pass-by-pointer throughout.

### 1.2 src/api-txn-data.c + api-cursor.c — PERF VERDICT (OPTIMAL)

Per-call tax of `mdbx_get` (api-txn-data.c:67-84), audited layer by layer:

| stage | cost |
|---|---|
| null checks | 2 predictable branches (api-txn-data.c:71) |
| `check_txn` | flag-bit tests + `pthread_self()` compare under TXN_CHECKOWNER (cogs.h:458-473; `osal_thread_self` = `pthread_self()`, osal/osal.h:277-286 — a TLS read, not a syscall) |
| `cursor_init` | `dbi_check` + `couple_init` ≈ 15 field stores; `cursor_couple_t` = **840 bytes of stack**, no memset (`VALGRIND_MAKE_MEM_UNDEFINED` is a no-op in normal builds) — measured via `sizeof` probe |
| lookup | one `cursor_seek` (MDBX_SET) — the tree walk itself; the branchless one-compare gate (`BINARY_BRANCHLESS_SEARCH_CYCLE`, LEM-V4-1) applies |

No redundant per-call validation: `dbi_state` `DBI_STALE` refresh is amortized
once per txn per dbi (cursor-setup.c:325-328), not repeated per call. The
840-byte stack couple is initialized by stores only — zeroing it would *add*
cost. `mdbx_put/del/replace_ex` add only the flag-mask checks (api-txn-data.c:
257-259, 361-367) — pure register tests.

api-cursor.c hot wrappers: `mdbx_cursor_get` = `cursor_check_ro` + `cursor_ops`
(api-cursor.c:454-460); `mdbx_cursor_get_batch` walks pages directly via
`node_read` per record with one `check_leaf_type` per page (guarded by
`MDBX_DISABLE_VALIDATION`, api-cursor.c:642) — the bulk API has no per-record
validation inflation. `scan_continue`'s per-record indirect predicate call is
inherent to the callback API; users needing more throughput have
`get_batch`.

Known V-037 (api-txn-data.c:301) is a correctness finding, not a perf one.

### 1.3 src/api-get-cached.c — PERF VERDICT (OPTIMAL, claims verified)

The reader-side perf feature. Costs measured/verified per class:

**HIT fast path (per-call hot)** — `mdbx_cache_get` (api-get-cached.c:309-378)
+ `cache_get` (:49-77). Assembly of the MT wrapper (gcc 15.2 -O2): one 32-byte
`vmovdqu` copy of the entry, `safe64_read` of `last_confirmed_txnid` (a **plain
load** on x86-64 — `atomic_load64(mo_AcquireRelease)` emits no fence and no
`lock`), three plain loads (trunk/offset/length), a 4-comparison stability
check plus a re-read, then the call. Inside `cache_get`: trunk>confirmed guard,
`check_txn`, `txn->txnid` comparisons, then `base + offset`/`length`. Total
≈ 60-70 instructions, **zero atomic RMW, zero fences** on the hit path; the
single `lock cmpxchg` in the whole function belongs to the publish path
(:364-371), reached only after a tree walk. The seqlock double-read design is
intentional (benign-torn-read detection, cf. the 7-status machine in
`fact:verification:api-surface`); `mdbx_cache_get_SingleThreaded` skips it
entirely for per-thread entries (:380-386).

**CONFIRMED path (per-call, reader newer than the entry)** — the "confirm is
cheap" claim **holds**: for an unmodified table the path is `dbi_check` +
`txn_basis_snapshot` (flag arithmetic, cogs.h:479-485) + one `DBI_STALE` test
+ `tbl_root_txnid` (table.c:6-17: a walk over the txn parent chain — one
iteration for a plain reader — plus a `mod_txnid` test) + two comparisons.
**No page access** in the stable case; `entry->last_confirmed_txnid` is
bumped in place (:144). The `cursor_init`/`check_key`/tree-walk (:154-304)
runs only when the table actually changed since the last confirmation.

**Confirm walk early-exit** — when the walk is needed, at each level it pays a
single `is_afterlast` key comparison (:44-47, 200-205, 249-254) to confirm a
frozen subtree and stop; `tree_search_branch` per level only along pages with
`txnid > last_confirmed`. Worst case equals a full uncached search — never
worse. The rightmost-subtree case (key > last key on every frozen page) is
the long path, bounded by tree depth ≤ CURSOR_STACK_SIZE.

**Fallback costs (documented, not defects)** — `MDBX_CACHE_BEHIND`
(txn older than trunk, :67-69) and `MDBX_CACHE_UNABLE` (ABA window, txnid
strictly inside `(trunk, last_confirmed)`, :87-95) fall back to a full search
through `cache_fallback` (one recursion with a stub entry). For a long-lived
shared entry and an old reader, every get pays the full search — this is the
documented correctness-preserving behavior, worth stating in user-facing docs
as "old readers lose the cache".

**Suggestion (docs-only, P-3)** — mdbx.h:5838-5839 says SingleThreaded is for
"single-threaded cases only, otherwise undefined". Per-*thread* cache entries
in a multi-threaded process are the classic high-throughput pattern and are
safe with the SingleThreaded variant (no concurrent writer of the entry);
the current wording reads as if a single-threaded process were required,
which would push MT users onto the (slightly costlier) MT wrapper needlessly.

### 1.4 src/api-range-estimate.c — PERF VERDICT (OPTIMAL)

- `cursor_diff` (api-range-estimate.c:13-77): O(divergence depth) ≤
  CURSOR_STACK_SIZE; the shared-prefix loop exits at the first differing
  level; the ±1 correction loops touch `page_numkeys` per level. No
  allocation.
- `estimate` (:79-142): fixed-point multiply loop with 4-powers batching
  (≤ height/4 iterations), clamped to `tree->items`.
- `mdbx_estimate_range` fast paths: FIRST..LAST returns `tree->items`
  directly (:262-265); single-key returns 1/items-count without a second
  cursor (:287-302); two-cursor case = two `SET_LOWERBOUND` searches +
  `cursor_diff` — inherent.
- `mdbx_estimate_move` builds a stack couple + one search; no heap.

### 1.5 mdbx++ slice/buffer/cursor/txn templates — PERF VERDICT (OPTIMAL, claim verified by codegen)

Evidence: `repro/v-perf-d-cxx-inline.cpp` compiled `-std=c++20 -O2`
(gcc 15.2, x86-64). Per function:

| probe | generated code |
|---|---|
| `txn::get` wrapper | load handle, zero-init the sret `slice` (inherent NRVO construction), `call mdbx_get@PLT`, `test`+branch, `throw_exception` **only on the cold error edge** |
| `cursor::move` wrapper | one 16-byte `vmovdqu` copy of the key into the returned `move_result` (the output member, not an avoidable copy), `call mdbx_cursor_get@PLT`, error switch, cold throw |
| `txn::get_cached` wrapper | **`jmp mdbx_cache_get@PLT`** — a pure tail call, zero instructions of its own |
| `operator==(slice,slice)` | inline length compare + `memcmp@PLT` |

- `slice` publicly inherits `::MDBX_val` (decl-slice.h++:22): all accessors are
  constexpr one-liners; `check_length` is one compare/throw (cold edge).
  **No hidden copies** anywhere: arguments are const-refs, `put` takes `slice`
  by value (16 bytes = 2 registers).
- Transcoders (`to_hex`/`from_base58`/…) are **non-virtual, template-dispatched
  at compile time**: exactly one `envisage_result_length()` + one
  `write_bytes()` call per conversion (impl-buffer.h++:55-112); the hex encode
  loop is branchless per nibble (mdbx.c++:643-664). No per-element virtual
  dispatch.
- `buffer` allocator traffic: SSO-style inplace storage
  (`bin::is_suitable_for_inplace`), `reshape` same-capacity fast path is
  pointer arithmetic + memmove **only when the headroom actually changes**
  (decl-buffer.h++:375-397); `default_capacity_policy::advise`
  (:144-163) doubles on grow capped at `max_reserve=65536`, keeps capacity on
  small shrinks (64-byte pettiness floor). No gratuitous reallocation.
- Exceptions in hot getters are UNLIKELY-gated cold edges
  (impl-exceptions.h++:54-96); there is no exception-unsafe pattern that
  forces extra checks on the success path.

Micro-note (P-2, the only code-level gap found in the C++ layer):
`cursor::get_batch` (impl-cursor.h++:154-182) allocates a staging
`std::vector<MDBX_val>` of `max_pairs*2` slots, then a second
`std::vector<pair>` with `reserve(count/2)` and a 32-byte-per-pair copy loop —
two heap allocations and one extra pass per batch call. Realistic gain of
collapsing to a single allocation (caller-provided staging span, or
documenting the C API `mdbx_cursor_get_batch` for zero-copy bulk loops):
~5-15% of the call for small batches (≤64 pairs), ~1-3% for large ones; risk
low but it is an API-shape change, so documentation of the C alternative is
the low-risk 80% win.

### 1.6 src/mdbx.c++ — PERF VERDICT (OPTIMAL)

- RAII: `txn_managed::~txn_managed` = one `MDBX_UNLIKELY(handle_)` test +
  `mdbx_txn_abort`; `env_managed`/`cursor_managed` analogous. The managed
  cost is the C call itself.
- `trouble_location` (mdbx.c++:36-190) is assembled only inside `bug`/assert
  paths (`__cold raise_bug`) — never on hot edges.
- Error construction: `error::success_or_throw` = one compare on success;
  message formatting lives behind the `[[noreturn]]` throw path only.

### 1.7 src/tools/*.c — PERF VERDICT (GAPS: 1, trivially fixable)

- **P-1: dump.c per-byte locked stdio** (`src/tools/dump.c:40-57`). `dumpval`
  emits every output byte via `putchar()` — two calls per byte in bytevalue
  mode plus one per record delimiter; each glibc `putchar` takes the stdio
  file lock. Frequency: per dumped byte (cold tool, but user-visible on
  multi-GB dumps). Cost: the encode loop is stdio-lock-bound, roughly
  2-4x slower than the same loop with unlocked calls; end-to-end it shows
  when the destination is a pipe or fast NVMe rather than a slow spinning
  disk. Recommendation: use `putchar_unlocked`/`putc_unlocked` (POSIX) /
  `_putchar_nolock` (Windows) via a tiny shim in tools/common.h, or assemble
  lines in a buffer and `fwrite`. Expected gain: 2-4x on the formatting
  loop; risk: none (the tool is single-threaded).
- load.c: `fgets`-based reading + per-record puts with 100K-item batch
  checkpoints (`batch_items`, load.c:526, 880-895) — commit amortization
  already correct; stdin parsing dominates, no trivially-fixable loss.
- stat.c/chk.c/defrag.c/copy.c: per-table or per-page I/O on genuinely cold
  paths; nothing trivially fixable beyond noise.

### 1.8 src/api-copy.c — PERF VERDICT (OPTIMAL)

- `copy_asis` (api-copy.c:509-652): kernel fast paths first — `sendfile`
  for pipes, `copy_file_range` for files (with the 5.3-5.12 kernel blacklist
  and ecryptfs guard), graceful per-chunk fallback to
  memcpy+`pwrite` in `MDBX_ENVCOPY_WRITEBUF = 1 MiB` chunks
  (options.h:258-260). Chunking and syscall amortization are right.
- `copy_with_compacting` (:354-505): double-buffered (`2 × 1 MiB`) writer
  thread with a condpair — write I/O overlaps the tree walk. Per page: one
  `page_copy` into the write buffer plus a **required** memset of the page
  slack (buffers are reused across pages, api-copy.c:415 vs :131, so the
  slack must be re-zeroed). `compacting_put_bytes` accumulates pages into
  1 MiB writes — batching is correct. The initial GC counting loop
  (:386-404) is O(GC records) and required by the used-pages verification
  (`gc_check_rowdata` per record) — validation-dominated, not fixable
  without dropping a check.
- Per-walk `osal_malloc` of the depth-scratch buffer (:172) is one
  allocation per (nested) tree per copy — cold-path negligible.

---

## 2. Findings summary

| id | site | frequency | cost | recommendation | expected gain | risk |
|---|---|---|---|---|---|---|
| P-1 | src/tools/dump.c:40-57 (`dumpval`) | per dumped byte (cold tool) | locked `putchar()` per byte (2/byte in bytevalue mode); stdio lock per call | `putc_unlocked`/`_nolock` shim or line-buffered `fwrite` | 2-4x on the encode loop (visible on pipe/fast-disk multi-GB dumps) | none |
| P-2 | mdbx++/impl-cursor.h++:154-182 (`cursor::get_batch`) | per batch call (hot bulk scans) | 2 heap allocs + one extra 32B/pair copy pass | caller-provided staging span (or document C `mdbx_cursor_get_batch` for zero-copy loops) | ~5-15% for small batches, ~1-3% large | low (API shape) — docs-first |
| P-3 | mdbx.h:5838-5839 (SingleThreaded doc) | per-call (doc-driven choice) | doc ambiguity pushes MT users to the seqlock wrapper; MT wrapper = +2 loads +32B copy +4 compares | clarify that per-thread entries are safe with `mdbx_cache_get_SingleThreaded` | a few ns/call for MT users; better: removes a wrong "must be single-threaded process" reading | none (docs-only) |

All other audited areas: **OPTIMAL** — no change recommended; the important
negative results are recorded as invariants below.

## 3. Established perf-invariants (valuable negative results)

- **INV-PERF-API1 (CRUD per-call tax is O(1), allocation-free)**: every
  `mdbx_get/put/del/replace/cursor_get` pays only null-checks + `check_txn`
  (flag tests + `pthread_self()` TLS read) + `cursor_init` (~15 stores into a
  840-byte stack `cursor_couple_t`, no memset, no heap). DBI_STALE refresh is
  once per txn per dbi. There is no per-call allocation anywhere in the CRUD,
  estimate or cached paths.
- **INV-PERF-CACHE1 (cache HIT is lockless)**: `mdbx_cache_get`'s hit path is
  32B entry copy + 2 acquire-loads (`safe64_read` = plain load on 64-bit
  targets) + 4 stability compares + base+offset; zero atomic RMW, zero fences;
  the single `lock cmpxchg` (publish, api-get-cached.c:364-371) is reached
  only after a tree walk. The stable-DB CONFIRMED path is likewise O(1)
  (`tbl_root_txnid` compare chain, no page access); the confirm walk descends
  only along pages newer than `last_confirmed_txnid` with a per-level
  one-compare (`is_afterlast`) early exit — worst case bounded by a full
  uncached search, never worse.
- **INV-PERF-CXX1 (C++ wrapper zero-overhead, machine-checked)**: probes in
  `repro/v-perf-d-cxx-inline.cpp` show at `-O2` exactly one C-API call per
  wrapper (`txn::get`, `cursor::move`), a pure tail call for
  `txn::get_cached`, inline slice operations, UNLIKELY-gated cold throw edges,
  and no hidden copies (`slice` derives from `MDBX_val`; by-value 16-byte
  passes compile to register moves). Transcoders are template-dispatched,
  non-virtual; buffer `reshape` has a same-capacity no-alloc fast path and a
  doubling capacity policy capped at 64 KiB reserve.
- **INV-PERF-COPY1 (copy pipelines use kernel fast paths and 1 MiB batched
  writes)**: `copy_asis` prefers `sendfile`/`copy_file_range` with graceful
  fallback; compacting copy overlaps I/O with the walk via a double-buffered
  (2×1 MiB) writer thread; the per-page slack memset is required (buffers are
  reused), not waste.

## 4. KB keys written by this tranche

- `bottleneck:tools:dump-locked-stdio` (P-1)
- `bottleneck:cxx:get-batch-staging` (P-2)
- `fact:verification:perf-api-invariants` (INV-PERF-API1/CACHE1/CXX1/COPY1)
