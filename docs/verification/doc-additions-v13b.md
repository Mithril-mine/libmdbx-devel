# V13b — C++ API headers (`mdbx++/`), statement-level verification (stream C)

Scope: `mdbx++/begin.h++`, `end.h++`, `decl/impl-{core,exceptions,transcoders,slice,buffer,env,txn,cursor}.h++`
(~11.3K lines) plus the non-inline counterparts in `src/mdbx.c++` and the C contracts they forward to.
Method: per-function P1..P7 (conditions, arithmetic/conversions, memory/lifetime, termination, exits,
ABI/ODR), machine-checked where reproducible — `docs/verification/repro/v13b-cxx-template-matrix.cpp`
(compile/instantiation matrix: {char, wchar_t} x {4, 8} ints x reference/allocated buffers) and
`docs/verification/repro/v13b-cxx-runtime-db.cpp` (end-to-end wrapper semantics against a real DB).
Both pass (gcc 15.2, `g++ -std=c++20`), library built from `src/*.c` + `src/osal/posix/*.c` +
`src/mdbx.c++ -DMDBX_BUILD_CXX=1`.

Verdicts: **PROVED** (proof complete), **PROVED-A** (proved modulo a stated assumption), **N-A**
(not analyzable here / C-side only). No REFUTED and no SUSPECT findings this tranche; the findings
below are ABI gaps, formal nits and documentation traps, none memory-unsafe.

---

## 1. `begin.h++` / `end.h++` — LIBMDBX_INLINE_API machinery, ODR

### begin/end/machinery — PROVED
- `LIBMDBX_INLINE_API` (mdbx.h:441-446): user builds get `static __inline NAME` (internal
  linkage — no ODR participation, per-TU copies are fine); library builds (`LIBMDBX_INTERNALS`)
  declare the exported `NAME` + define `static __inline __inline_NAME`, with hand-written exported
  fallbacks in `src/api-cold.c:529-616` and `src/api-get-cached.c:391` under
  `#ifndef LIBMDBX_NO_EXPORTS_LEGACY_API`. All 27 inline functions audited — exactly one lacks an
  exported fallback (see F-V13B-1).
- `MDBX_EXTERN_API_TEMPLATE` / `MDBX_INSTALL_API_TEMPLATE`: extern declarations
  (`impl-buffer.h++:37-43`, guarded `!(MinGW) || LIBMDBX_EXPORTS`) match explicit instantiations in
  `mdbx.c++:1179/1182` (unguarded). The asymmetry is correct: MinGW *user* builds skip the extern and
  instantiate implicitly; only the library (MinGW+EXPORTS) must emit them.
- `static` strlen/memcpy/memcmp declared at namespace scope in a header: internal linkage, no ODR
  hazard. `memcmp` runtime path guards `bytes==0` (avoids the null-pointer-with-n=0 formal UB);
  constexpr paths are byte loops.
- `std::hash` specializations are for library-owned types (legal); `std::to_string` overloads are
  ostringstream-based and allocate — fine.
- `end.h++:26 operator<<(ostream&, MDBX_error_t)` — inline in `mdbx::`, converts via `error`; OK.
- `MDBX_INLINE_API_ASSERT` is active only under MDBX_CHECKING/MDBX_DEBUG (mdbx.h:646-655); all uses
  are invariants that also hold in release (checked individually below).

### slice (decl-slice.h++ / impl-slice.h++) — PROVED (grouped)
- Constructors: `slice(ptr, bytes)` = `ptr ? check_length(bytes) : check_nullptr_length(bytes)`;
  `check_length` throws `max_length_exceeded` for `bytes > MDBX_MAXDATASIZE (0x7fff0000)`,
  `check_nullptr_length` throws `bad_value_size` for non-zero. So `slice(nullptr, N>0)` throws;
  `slice::blank_for_put_reserve(N)` is the only (null, N) spelling and still validates N against the
  max. All string/string_view ctors multiply `length()*sizeof(CHAR)` — no overflow for any sane
  string; the array-literal ctor asserts `text[SIZE-1]=='\0'` (debug) and refers `SIZE-1` bytes.
- **safe_middle (#28 fix) — PROVED, arithmetic complete**: `n > max_length` → max_length_exceeded;
  `from > max_length || from + n > size()` → out_of_range. Both operands are bounded by
  `max_length = 0x7fff0000`, hence `from + n <= 0xfffe0000` — **no size_t overflow on 32- or
  64-bit**; the `from > max_length` clause is redundant (any slice length <= max_length makes it
  imply `from > size()`) but harmless. Machine-checked in the matrix repro including the
  `from = n = max_length` corner.
- `try_as_pod (#144 fix) — PROVED`: size mismatch returns `std::nullopt`, match returns the copied
  POD; `as_pod` throws `bad_value_size` (noreturn `throw_bad_value_size`, so no fall-off-end UB).
  Both reject non-standard-layout and pointer types via static_assert.
- `substr`/`safe_head`/`safe_tail`/`safe_remove_*`: `pos > size()` guarded, `min(count, size-pos)`
  cannot overflow; `find/rfind/find_*_of` families: loops bounded by `length()-needle.length()`
  (safe since `needle.length() <= length()` is established first), all memcmp reads in bounds.
  `compare_fast/compare_lexicographically` use `intptr_t(len_a) - intptr_t(len_b)` — for any
  well-formed slice `len <= 0x7fff0000` so no signed overflow (a slice with a larger length cannot
  be produced through the public ctors).
- `assign(ptr, bytes)`: sets `iov_base` BEFORE the length check, so a throwing call leaves
  `iov_base` updated and `iov_len` stale (see N-V13B-3; object remains destructible and is_valid()
  == false only if the old length was 0 — formal exception-safety nit, not reachable UB).
- `iov_len/iov_base` invariants: `is_valid() == !(null base && len != 0)`; `invalidate()` keeps the
  length (matches the library's moved-from convention, documented); `clear()` zeroes both; `set_end`
  asserts monotonic ptr and `check_length` catches a wrapped negative.

### buffer (decl-buffer.h++ / impl-buffer.h++) — PROVED / PROVED-A
- **Modality state machine**: modality is derived from pointer geometry (`is_freestanding()` =
  `base` within `[silo_begin, silo_begin+capacity]`), with the in-place/allocated switch carried by
  the `bin` union signature byte (`lastbyte_ == 0xFF` = inplace; allocated capacity's top byte is
  the poison 0 since `max_capacity < inplace_signature_limit`). Every mutating op re-derives the
  modality before touching storage; no cached stale flag exists, so the #24 move-assign trap cannot
  recur.
- **Move ctor / move-assign (incl. `assign(buffer&&)`)**: the self-referencing-source guard
  (src is a reference INTO this storage) memmoves the bytes to the storage start before anything is
  released; otherwise `inherited::assign(std::move(src))` copies the slice first, headroom/data/
  length are captured BEFORE the source invalidates, and:
  - non-propagating allocator + allocated src: re-allocate with the own allocator via
    `reshape<true>` while the src allocation is still alive (src keeps and frees it later);
  - otherwise `propagate` (release own storage with the OLD allocator, then adopt) followed by
    `move_content` (steal pointer / copy inplace bin / no-op for reference) with the inplace fixup.
    No leak, no double-free, no use-after-release on any path; exception window limited to the
    reshape copy with the basic guarantee. `silo(silo&&, bool)` and `exchange()` verified per
    modality pair (inplace/inplace uses a temp silo; any-reference path calls move_content in an
    order that is no-op for references; both-allocated swap propagates allocators or throws
    allocators_mismatch BEFORE any mutation).
- **reshape\<external_content\>**: capacity transition table verified — fast path (same capacity,
  memmove only when headroom differs or content external), allocated->inplace (copy then
  deallocate), inplace->allocated (copy BEFORE `make_allocated` overwrites the union — the source),
  allocated->allocated (swap pointers, copy from the old allocation, deallocate old with the OLD
  capacity). The `is_suitable_for_inplace` branch asserts `is_allocated()`; unreachable for an
  inplace bin (advise returns `current` for any `wanna <= inplace` and a value `> inplace_capacity`
  otherwise), i.e. the assert cannot fire — proved by case analysis on `default_capacity_policy::advise`.
- **reserve()**: the headroom/tailroom nudges round up to `pettiness_threshold` only when below
  `max_length - pettiness` (no overflow); `check_length(h, l, t)` chains bounded sums
  (each part <= max => sum <= 2*max < SIZE_MAX, outer check throws). Post-conditions
  `headroom() >= wanna && tailroom() >= wanna_tailroom` hold because the advised capacity is >= the
  requested whole capacity.
- **append/add_header**: aliasing discipline — `aliased` (src wholly inside the content) survives a
  reallocation via the saved offset; src inside the own storage but NOT wholly inside the content is
  copied out first (`const buffer chunk(...)`) before the storage is relocated; `check_length(bytes)`
  fires before any mutation; `iov_len + bytes <= max_length` is enforced by the reserve path's
  `check_length`. `reaches_tailroom()` (uintptr comparisons) correctly detects overlap with
  `[end, silo_end)` and the `append_hex/base58/base64/decoded_*` wrappers copy such a source to a
  temp buffer first; custom producers get the documented precondition.
- **#137/#142 guards**: `buffer(nullptr_t, INT)` and `buffer(nullptr_t, INT, bool)` templates with
  `check_nullptr_length` reject non-zero lengths at run time (compile-time ambiguity avoided by the
  enable_if integral, non-bool templates); `buffer(nullptr, 0[, ref])` compiles and yields a null
  empty buffer. Machine-checked in the matrix repro (all forms compile; N>0 forms throw
  bad_value_size).
- `data_preserver` (extract/replace/replace_reserve): callback copies into the owned buffer and
  returns `MDBX_RESULT_FALSE` (=0, replace proceeds; C side aborts the put on any non-SUCCESS return
  — api-txn-data.c:439-440) and on exception captures into the thunk and returns `MDBX_RESULT_TRUE`
  to abort, then `success_or_throw(rc, thunk)` rethrows the ORIGINAL exception. Verified against the
  C contract (mdbx.h:5993-6057 "returned as is").
- `clone` preserves headroom/tailroom (contents of the rooms are uninitialized, as reserve implies);
  `make_inplace_or_reference` references allocated content (documented); `buffer_pair_spec`
  move-ctor's `noexcept(move_assign_alloc::is_nothrow())` is formally inaccurate for allocators with
  throwing move-CTOR but noexcept move-assign (N-V13B-4).
- `buffer(nullptr_base, len)` reference edge: `is_freestanding()` uses a wrapping size_t pointer
  difference — for any real external pointer the difference exceeds `capacity` (static-asserted
  margin vs `max_length`); comparisons of unrelated pointers are formally unspecified but universal
  practice (PROVED-A).

### core (decl/impl-core.h++) — PROVED (grouped)
- `cache_entry`: move = copy + reset(source); `operator bool` = confirmed flag; all constexpr.
  `map_handle::info::key_mode()/value_mode()`: masks map flag combos to scoped enums; flag
  combinations without an enumerator (e.g. `INTEGERKEY|REVERSEKEY`, `DUPSORT|INTEGERDUP` w/o
  DUPFIXED) produce out-of-range scoped values (formally UB per [dcl.enum], practically the tag is
  inert — N-V13B-2). `is_usual/is_ordinal/is_samelength/is_reverse` predicates match the flag bits;
  `key_mode::msgpack = -1` compares via its own enumerator (safe under unsigned promotion).
- `error::is_failure()` = `code != 0 && code != -1` — correct given
  `MDBX_RESULT_FALSE == MDBX_SUCCESS == 0`, `MDBX_RESULT_TRUE == -1`.

### exceptions (decl/impl-exceptions.h++ + mdbx.c++) — PROVED
- rc→class mapping (`error::throw_exception`, mdbx.c++:370-425): EINVAL→invalid_argument,
  ENOMEM→bad_alloc, SUCCESS/RESULT_TRUE→logic_error, then 36 CASE_EXCEPTION pairs; the default
  falls back to base `mdbx::exception` for MDBX-range codes (`is_mdbx_error` gate) and
  `std::system_error` otherwise — mapping is total. The only MDBX-specific code with no dedicated
  class is `MDBX_BACKLOG_DEPLETED` (falls to the base class; users cannot catch it specifically —
  deliberate, note for docs).
- `what()` = MDBX-symbol name; `message()` = `mdbx_strerror_r` with a 1 KiB buffer and null-guard.
- `boolean_or_throw`: `RESULT_FALSE`(0)→false, `RESULT_TRUE`(-1)→true, anything else thrown — the
  `SUCCESS == RESULT_FALSE` aliasing is intentional and load-bearing (e.g. `txn_managed::amend`
  returns `!boolean_or_throw(rc)` so rc==0 means "amended").
- `exception_thunk::capture()` asserts cleanliness; all wrapper thunks (readers, tables, gc_info,
  defrag, scan, preserve) return a stop/abort code after capture and `rethrow_captured()` runs
  after the C call — verified for each of the six thunks (P5 complete).

### transcoders (decl-transcoders.h++ + mdbx.c++) — PROVED
- `envisage_result_length` are upper bounds: hex `2*n (+ wraps)`, base64 `4*ceil(n/3) (+ wraps)`,
  base58 `n` (1:1 worst case), wrap accounting `bytes + bytes/wrap_width >= ceil((bytes-1)/wrap)`
  newlines. `write_bytes` re-checks `envisage > dest_size` → `throw_too_small_target_buffer`.
  The 32-bit size_t overflow of base58's `n*11+7` is unreachable in practice (needs a ~2 GB value
  with a 32-bit size_t; N-V13B-1).
- `from_hex`: **remainder check** — odd length throws domain_error unless `ignore_spaces`; pairs
  validated by isxdigit; decode `hi=(c|0x20)-'a'; hi += 10 + ((hi>>7)&39)` proven for 0-9/a-f/A-F;
  `isspace/isxdigit` receive unsigned byte values (no negative-int UB). `is_erroneous()` treats an
  empty (or all-spaces) input as erroneous — `is_hex("") == false` while `hex_decode("")` succeeds
  empty (asymmetry, doc note).
- `from_base64`: length%4 check with the same ignore_spaces exception; final-group padding rules
  (`(a|b)>=0 && d==EQ`, then `c>=0` → 2 bytes / `c==EQ` → 1 byte); invalid chars detected via the
  b64_map sign; the 3-byte scratch write for a 1-byte group stays inside the envisaged buffer.
- `from_base58`: leading run handles `'1'`→0x00 and ignorable spaces, any other `<= '1'` char
  throws; decode write length `<= 47*n/64 + leading zeros <= envisage = n`; bignum limbs are
  base-58^9 (58^4 on 32-bit) with 56-bit (24-bit) scratch — limb bounds verified
  (58^9 = 0x1A636A90B07A00 < 2^56; 58^4 = 0xACAD10 < 2^24), encode `%`/`/` vs decode
  `&(2^56-1)`/`>>56` both exact for the true limb values.
- `to_base58/to_base64/to_hex::write_bytes` and `::output` agree with the envisage bounds; ASSERTs
  are debug-only and consistent.

### env (decl/impl-env.h++ + mdbx.c++) — PROVED
- **Scoped-enum breaking change**: `enum class mode/durability : unsigned` with the same value
  order as the old unscoped enums; all 8 old names kept as `[[deprecated]] static constexpr`
  members of `env` with identical values, including the `nested_transactions = write_file_io`
  alias. `mode2flags`/`make_flags`/`durability_from_flags` throw `std::invalid_argument` on
  out-of-range casts; `make_flags` maps every option 1:1 (`MDBX_NOSTICKYTHREADS` take included),
  clears `MDBX_WRITEMAP` for nested transactions, and skips durability/LIFO bits for readonly —
  round-trip `make_flags` ↔ `*_from_flags` verified in the matrix repro.
- `get_preopen_snapinfo` (#182): all five path overloads forward the new `MDBX_preopen_flags mode`
  (default `MDBX_PREOPEN_PROPERLY`) to `mdbx_preopen_snapinfo_ex[_W](pathname, mode, &info, sizeof)`
  — argument order verified against mdbx.h:7693/7700; filesystem overload uses `native()`
  (wide on Windows → the W entry).
- **chk() tables lifetime**: wrapper passes a zeroed `silent_chk_callbacks` when `cb == nullptr`,
  throws only for real failures (`MDBX_PROBLEM` with counted problems is a result, not an error) —
  and the documented contract "`ctx.result.tables` valid only inside callbacks, `nullptr` after
  return" matches the C side (chk.c:799 sets it, chk.c:316 nulls it as "points into the freed chk").
- `sync_to_disk` maps SUCCESS/RESULT_TRUE→true, BUSY→false, else throws; `set_sync_period__seconds_double`
  rejects NaN/negative/overflow (`!(s>=0 && s*65536 <= UINT_MAX)`); `limits::*` convert negative
  intptr results to EINVAL throws; `enumerate_readers`/`defrag` thunks handle re-entry after a
  visitor exception (defrag returns abort; rethrow after the C call); `start_write(dont_wait=true)`
  throws MDBX_BUSY while `try_start_write()` returns an empty handle — asymmetry is intended but
  worth a doc sentence.
- `env(env_managed&&) = delete` (and the assignment) closes the slicing hole where an owning
  temporary would be dropped unclosed.

### txn (decl/impl-txn.h++) — PROVED
- **put flag forwarding**: put_mode insert_unique/upsert/update → MDBX_NOOVERWRITE/UPSERT/CURRENT
  (values asserted equal in the matrix repro); append → MDBX_APPEND[|APPENDDUP]; MULTIPLE protocol
  `args[0]={array,len}, args[1]={null,count}` matches `mdbx_put`/`mdbx_cursor_put`; partial
  KEYEXIST without allow_partial poisons the txn (`mdbx_txn_break`) before throwing — matches the
  C contract that some items may already be stored.
- Reserves: `blank_for_put_reserve` → put with `MDBX_RESERVE`; the C API fills `iov_base` (runtime
  verified); `try_*_reserve` return the still-blank slice on failure (value_result.done==false,
  the payload must be ignored — doc note).
- `get_ex` (get with values_count) throws NOTFOUND on an absent key (no absence overload with
  count has an absence default — the 2-arg get handles that); `get_equal_or_great` 1-arg throws
  NOTFOUND, 2-arg returns the absence value; done flag compares only keys (documented for dupsort).
- `replace(map, key, old_value, new_value)` uses `MDBX_CURRENT|MDBX_NOOVERWRITE` — the documented
  dupsort pair-selector combination (mdbx.h:6030-6035). Trap documented in §4: with the NULL
  preserver the C side fills `old_data` only when the old value is on a NON-modifiable page
  (api-txn-data.c:431-441); on a dirty page old_value is left untouched — use the `replace<BUFFER>`
  form when the old value is needed.
- `extract/replace/replace_reserve<BUFFER>` preserver protocol — see buffer section; the exception
  path aborts the put (C aborts on non-zero preserver return) and rethrows the original exception.
- **txn_managed RAII**: destructor aborts best-effort (never throws); `abort/commit` guard
  `MDBX_TXN_HAS_CHILD` (throws BAD_TXN, handle kept), reset the handle except on
  THREAD_MISMATCH (state unchanged — retry from the right thread), commit treats RESULT_TRUE as
  "aborted-instead" and throws BAD_TXN only if the txn was already ERRORED; `checkpoint` keeps the
  handle for THREAD_MISMATCH/EINVAL/BAD_TXN and resets otherwise; `commit_embark_read` passes
  `&handle_` (in/out reuse). `amend`: `!boolean_or_throw` — rc==0 (SUCCESS==RESULT_FALSE) means the
  write txn started, RESULT_TRUE means a newer snapshot existed and nothing happened, BUSY+dont_wait
  → false; `&handle_` in/out is preserved on all error returns (api-txn.c:750-752 nulls `*ptxn`
  only when it differs from rtxn).
- `clone(context)` → `mdbx_txn_clone(origin, &ptr, NULL→ptr)` create-new; `clone(txn_managed&, ctx)`
  requires a non-empty target and is the renew-into form (C accepts null to create-new — the C++
  wrapper narrows it deliberately). Behavior verified at runtime: cloning a same-thread RO txn
  without MDBX_NOSTICKYTHREADS → BAD_RSLOT (documented C restriction).
- park/unpark (#104): thin forwards; the C-side contract (AUTOUNPARK not surviving reset+begin,
  auto-unpark-only in check) is already covered by INV-T9 (fact:verification:api-surface).
- `sequence(map, increment)` throws overflow_error on RESULT_TRUE; `drop/clear/rename_map(name,
  throw_if_absent)` open with MDBX_DB_ACCEDE and map NOTFOUND/BAD_DBI to false-or-throw.

### cursor (decl/impl-cursor.h++) — PROVED
- `move_operation` mirrors `MDBX_cursor_op` 1:1 (`pair_exact = pair_equal` alias kept);
  `move(op, key, value)` (no throw_notfound) is `= delete` to force explicitness.
- **String-literal binding**: `erase(const char (&)[N])` binds to the slice overload (array → slice
  reference is a better match than array→pointer→bool); `erase(char* ptr)` and `erase(const char*)`
  are intercepted by the dedicated `CHARS/BOOL` template so a pointer never converts to the
  `whole_multivalue` bool; pair-of-literals overloads exist. Machine-checked in the runtime repro.
- `compare_position`: the int16 range check is exactly the C incomparability encoding
  (`±(1<<15)*k`, api-cursor.c:289) and legitimate ki-deltas cannot reach that magnitude
  (page item counts << 2^15); antisymmetry asserted (debug).
- `get_batch`: NOTFOUND/ENODATA → empty + `is_last=true`; RESULT_TRUE → last chunk; pair vector
  built from the interleaved MDBX_val array with the `i+1 < count` guard; requires a MULTIPLE-family
  op (get_current → EINVAL — runtime-verified, doc note).
- `put_multiple_samelength` requires DUPFIXED (else INCOMPATIBLE — runtime-verified); partial
  KEYEXIST handling mirrors txn::put_multiple.
- `distribute(vector<cursor>&)` — `static_assert(sizeof(cursor) == sizeof(MDBX_cursor*))` lets the
  vector double as the C pointer array; the const_cast is safe because the C entry positions the
  existing cursors and never rewrites the array; empty vectors short-circuit to EINVAL without
  `vector[0]` UB.
- `cursor_managed`: move-assign closes the held handle best-effort (noexcept, assert distinct),
  destructor closes via `mdbx_cursor_close2` (also resets inner state), `withdraw_handle` for
  hand-off; `close()` throws on failure but still nulls the handle after close2 returns success.

### ODR/ABI sweep — PROVED
- Every inline function in the headers is `inline`/`static`/template — no non-inline ODR-using
  definition outside `mdbx.c++`/`api-cold.c`; the two explicit buffer instantiations match the two
  extern declarations; `LIBMDBX_API_TYPE` classes have out-of-line key functions in `mdbx.c++`
  (no vague-linkage duplication); `#pragma warning` scopes are balanced; `dist-cutoff` markers are
  intact (no C-side leakage into the C++ namespace blocks).

---

## 2. Findings

| id | severity | area | note |
|---|---|---|---|
| F-V13B-1 | Low (ABI) | mdbx.h LIBMDBX_INLINE_API | `mdbx_txn_release_all_cursors` (2-arg legacy form, mdbx.h:6331) is the ONLY inline API without an exported fallback: it was a real `LIBMDBX_API` function until a3265e11 made it inline-only; every other legacy inline has an `api-cold.c`/`api-get-cached.c` fallback. A pre-a3265e11-linked binary that expects the export fails against the current lib, and `LIBMDBX_INTERNALS` builds declare an export that no TU defines. Fix: add `LIBMDBX_API int mdbx_txn_release_all_cursors(...) { return __inline_mdbx_txn_release_all_cursors(...); }` next to the others in api-cold.c. |
| N-V13B-1 | Info | transcoders | `to_base58::envisage_result_length`'s `length*11+7` overflows 32-bit size_t for ~2 GB values (unreachable in practice: such values exceed practical map sizes; the guard in write_bytes still prevents any memory unsafety). |
| N-V13B-2 | Info (formal) | core | `map_handle::info::key_mode()/value_mode()` may produce out-of-range scoped-enum values for flag combinations without an enumerator ([dcl.enum] UB, inert tag in practice). |
| N-V13B-3 | Info (formal) | slice | `slice::assign(ptr, bytes)` assigns `iov_base` before the length check; a throwing call leaves a mixed state (base updated, len stale). Suggest computing the checked length before the stores. |
| N-V13B-4 | Info (formal) | buffer_pair_spec | move-ctor `noexcept` uses `move_assign_alloc::is_nothrow()` while members are move-CONSTRUCTED (correct trait: `is_nothrow_move_constructible<allocator_type>`); identical for stateless allocators (all real uses). |
| N-V13B-5 | Doc | slice | `is_hex("")==false` while `hex_decode("")` succeeds empty; `slice::dump_val` output can be truncated for `<nullptr.N>` with N >= 3 digits (snprintf-bounded, memory-safe; debug ASSERT may fire). |
| N-V13B-6 | Doc | txn | `try_insert_reserve/try_update_reserve` return the STILL-BLANK slice when done==false; `txn::replace` (non-template) leaves `old_value` untouched when the old value resides on a dirty page (NULL preserver) — use `replace<BUFFER>` when the old value is needed. |

No REFUTED verdicts: nothing in the verified surface produced a counterexample; both machine-checked
repros pass.

## 3. Behavioral contract notes for `docs/engineering/cxx-api.md` (slice/buffer/cursor/txn templates)

1. Buffer modality is derived from pointer geometry — there is no stored flag; an EMPTY buffer is
   null/reference modality (`content_modality()==reference`), inplace only after content is stored
   inside the bin.
2. `buffer::append/add_header/append_*_producer` accept sources aliasing the buffer's own content
   or storage (self-append is safe); a custom producer must not read from the destination tailroom.
3. Dupsort CRUD semantics (all runtime-verified): `insert()` (NOOVERWRITE) fails KEYEXIST when the
   KEY exists even for a NEW multi-value — add dups with `upsert()`; `mdbx_put` fills the value
   with the PRESENT data on KEYEXIST (`try_insert`'s payload); `upsert` on dupsort ADDs a duplicate.
4. A parent txn cannot operate while a nested child is active (BAD_TXN); `txn_managed` refuses
   commit/abort with a pending child (BAD_TXN, handle kept).
5. `txn::clone()` of a same-thread RO txn requires MDBX_NOSTICKYTHREADS (else BAD_RSLOT);
   `clone(txn_managed&)` is the renew-into form and requires a reset RO handle.
6. `cursor::get_batch` accepts only the batch-samelength op family; `put_multiple_samelength`
   requires DUPFIXED; reserve stores the WHOLE reserved length (fill it completely).
7. `get_equal_or_great` 1-arg throws not_found on absence; the 2-arg overload returns
   `value_at_absence` with done==false. `get(map,key,count)` throws not_found on absence.
8. `txn_managed::amend` returns true when the write txn was started (rc==SUCCESS aliasing
   RESULT_FALSE), false when a newer snapshot exists or BUSY+dont_wait.
9. `env::chk` results: `ctx.result.tables` is only valid inside the callbacks (the C side nulls it
   at return); `MDBX_PROBLEM` with counted problems is a result, not a failure.
10. The deprecated `env::readonly`-style aliases are values of the NEW scoped enums (same numeric
    order); `env::mode::nested_transactions == env::mode::write_file_io` by design.

## 4. Test-coverage gaps (template-instantiation matrix follow-ups)

Covered by the two repros: {char, wchar_t} strings/views x slice/buffer; {4, 8}-byte keys via
`key_from*`; reference/inplace/allocated/clone/swap/move for std::allocator and a stateful
polymorphic-style allocator; safe_middle/try_as_pod/nullptr guards; deprecated aliases and
make_flags round-trip; the full CRUD/replace/extract/preserve/cursor/batch/clone/park/nested/RAII
surface against a live DB.

Gaps worth adding later:
- `wchar_t`-path env::open (Windows-only entries — not exercisable on Linux);
- multi-threaded txn ownership (THREAD_MISMATCH paths of abort/commit handle retention);
- `cursor::scan_until/fullscan` predicate-exception capture (thunk verified statically only);
- `env::defrag` visitor milestones and `enumerate_readers` exit codes against a populated DB;
- stateful allocator with `is_always_equal == false` for the allocators_mismatch throw path of
  `buffer::swap`/`assign`;
- `MDBX_MULTIPLE` partial store (`allow_partial=true`) returning `args[1].iov_len`.
