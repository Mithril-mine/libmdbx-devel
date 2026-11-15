# Verification tranche V13a, stream B — C++ API core (`src/mdbx.c++`) — proposed doc additions

Scope: `src/mdbx.c++` (2016 lines, 106 functions, statement-level) with the
inline contract cross-checks against `mdbx++/decl-*.h++` / `impl-*.h++` and the
C-core entry points the wrappers build on (`mdbx_env_close_ex`,
`mdbx_txn_{abort_ex,commit_ex,checkpoint,commit_embark_read,amend}`,
`mdbx_cursor_close2`, `txn_done_cursors`/`cursor_eot`/`cursor_drown`,
`check_txn`/`check_txn_anythread`).

Machine-checked (ASan+LSan, g++ 15, `-std=gnu++23`, tree @ `8db0b3ec`):
* `docs/verification/repro/v13a-checkpoint-has-child-control.cpp` — E1–E7
  lifecycle matrix, all green;
* `docs/verification/repro/v13a-transcoder-fuzz.cpp` — 21105 transcoder runs
  (bounds+canary+roundtrip), 60000 adversarial decoder-agreement cases,
  ~80 000 `is_printable` cases, all green except the two documented
  divergences below;
* throw-away probes (commands in §5): 50× failed `env_managed` construction
  under LSan (no leak), self-move of `env_managed`/`txn_managed`/
  `cursor_managed` (no double-close).

Verdict totals: no memory-safety or lifetime defect found in the file.
Three findings, all low: F-V13A-1 (stale commit-message claim, doc hazard),
F-V13A-2 (`is_erroneous` empty-source inconsistency), F-V13A-3
(`is_printable(disable_utf8=true)` accepts `0xE0–0xEF`). One REFUTED
verification hypothesis (checkpoint child-UAF) documented as a control
experiment.

Reused prior results: V-037 (registered-cursor discipline, C side),
INV-T9 (parked gate), V-045 (clc pin), wave-2 C-API dispatch verdicts
(mdbx_txn_begin/commit/abort), #97/#105/#77/#137/#142–146 anchors.

---

## 1. Contracts worth adding to `docs/engineering/cxx-api.md` (exception semantics per method)

### 1.1 Managed-handle lifecycle machine (`env_managed` / `txn_managed` / `cursor_managed`)

The wrappers share one rule, now verified at statement level for every exit:

* **Null on death, keep on mismatch.** After a finalizing C call the wrapper
  nulls `handle_` exactly when the C API may have freed the handle, and keeps
  it otherwise. Per handle:
  * `env_managed::close()`: `MDBX_BUSY` is returned by `mdbx_env_close_ex`
    **before** the signature CAS — the env is *not* released, `handle_` is
    kept (comment + code agree, api-env.c:609-…); `MDBX_EBADSIGN` (pre-CAS)
    leaks the env by definition of "signature broken", the wrapper nulls to
    prevent destructor re-entry; **every other code is post-CAS and the C
    function frees the env unconditionally** (`osal_free(env)` tail) — the
    wrapper nulls before throwing (`src/mdbx.c++:1379-1402`).
  * `txn_managed::commit()/abort()`: per the documented C contract
    ("a result other than MDBX_THREAD_MISMATCH means the transaction is
    terminated", mdbx.h ~4604/~4860) `handle_` is nulled for every code
    except `MDBX_THREAD_MISMATCH`, where the txn is alive but break-marked
    (`mdbx_txn_break` set `MDBX_TXN_ERROR`). `commit()` additionally treats
    `MDBX_RESULT_TRUE` as success when the txn was not pre-errored
    (`NOSUCCESS_PURE_COMMIT` empty commit) and as `bad_transaction` when it
    was; `was_errored` is sampled **before** the C call.
  * `txn_managed::checkpoint()`: the opposite policy is deliberate —
    `mdbx_txn_checkpoint()` does NOT end the transaction on any of its
    success paths, and of its error paths only the (unreachable-from-C++,
    see 1.2) nested-chain branch frees it. The wrapper therefore keeps
    `handle_` for `MDBX_THREAD_MISMATCH | MDBX_EINVAL | MDBX_BAD_TXN` and
    nulls it for anything else (the nested-child-commit-failure branch is
    the only reachable nulling case from plain C use).
  * `txn_managed::commit_embark_read()`: the C function updates `*ptxn` on
    every path where the old handle dies (`*ptxn = nullptr` for the
    nested-failure and basal/nested failure paths, the new RO handle on
    success) and leaves it untouched on the alive paths (entry checks,
    `MDBX_THREAD_MISMATCH`); the wrapper passes `&handle_` directly, so the
    field always ends up consistent — verified per-path in api-txn.c:336-414.
  * `txn_managed::amend()`: the C function writes `*ptxn` only on success
    (`*ptxn = env->basal_txn`), leaves the RO handle alive on
    `MDBX_RESULT_TRUE`/`MDBX_BUSY`/all entry failures; on the
    `txn_ro_reset`-failure path the caller receives an error while its RO
    handle is finished-but-alive. The wrapper maps `MDBX_BUSY`+`dont_wait`
    to `false` and everything else through `boolean_or_throw`.
* **Ctor exception safety by delegation.** Every `env_managed`/`txn_managed`
  constructor delegates to `X(create_env())` / `X(ptr)` and throws only from
  the delegating body. When the delegating body throws after the delegated
  ctor completed, the object's destructor runs (C++ [except.ctor]; verified
  empirically), so `~env_managed()` closes the env — a failed
  `mdbx_env_open` leaks nothing (machine-checked, 50 iterations under LSan).
* **Self-move is a no-op** for all three managed move-assignments
  (`if (this != &other)` guard; machine-checked). Moved-from wrappers hold
  `nullptr` and are destroyable; in `MDBX_CHECKING`/debug builds the base
  `txn`/`cursor`/`env` destructors poison `handle_` to `0xDeadBeef` so
  use-after-move crashes loudly instead of reading freed C objects.
* **Double-finalize throws, never UB.** Second `txn_managed::abort()` on a
  nulled handle → `mdbx_txn_abort_ex(nullptr)` → `MDBX_EINVAL` →
  `std::invalid_argument` (machine-checked). Second `cursor_managed::close()`
  on a nulled handle → `MDBX_EINVAL` likewise.
* **Throwing destructors terminate.** The destructor's best-effort
  `mdbx_*_close/abort` can still throw (`fatal` on `EBADSIGN`,
  `thread_mismatch` on a wrong-thread close of a sticky cursor). Documented
  consequence: if `close()` threw and the user ignores it, destruction
  ends in `std::terminate`. The `fatal` class additionally terminates when
  the exception object dies (`fatal_countdown`), i.e. any `MDBX_EBADSIGN` /
  `MDBX_PANIC` reaching the wrapper is process-fatal by construction.
* **`txn_managed` slicing ban (#77).** `txn(txn_managed&&)` is deleted;
  copying from an lvalue `txn_managed` stays allowed and yields a
  non-owning `txn` view. Base-class move-assign cannot drop ownership.

### 1.2 Child-transaction protection: commit/abort guard (C++), checkpoint guard (C core)

`txn_managed::commit()/abort()` refuse a parent with a live nested child with
`mdbx::bad_transaction` before touching the handle (4693ccca, issue #92) —
machine-checked (E2/E3). `txn_managed::checkpoint()` needs and has **no**
C++-side guard: `mdbx_txn_checkpoint()` checks its argument with
`check_txn(txn, MDBX_TXN_BLOCKED - MDBX_TXN_PARKED)`, whose `bad_bits`
include `MDBX_TXN_HAS_CHILD`, so the nested-chain commit/abort inside the C
function is unreachable for any handle with an attached child; the C++ call
throws `bad_transaction` and both handles stay alive (machine-checked, E1).
This differs from `mdbx_txn_commit_ex()` (entry `bad_bits = MDBX_TXN_FINISHED`
only, nested branch reachable — hence the C++ guard) — recorded because the
commit message of 4693ccca states the opposite ("Like checkpoint() already
does…", see F-V13A-1).

### 1.3 Cursors owned by ended transactions (`cursor_managed` across txn end)

A `cursor_managed` that outlives its transaction is safe by engine design
(machine-checked, E7): at EOT `txn_done_cursors()` → `cursor_eot()` drowns
each registered cursor (`cursor_drown` clears `txn`/`tree`/`dbi_state`,
cursor.h:549) and marks it `cur_signature_ready4dispose`; a later
`mdbx_cursor_close2()` takes the ready4dispose branch (both `txn` and
`backup` are null → drown + free, SUCCESS). Therefore:
* `txn_managed::commit()/abort()` may leave user `cursor_managed` objects
  alive: they become *detached* cursors, destructors close them cleanly, no
  UAF, no double-free — the C++ counterpart of V-037 ("wrappers never leave
  a registered cursor" holds: the engine itself unregisters at EOT);
* using a detached cursor for data operations is rejected by signature/state
  checks (`wait4eot` → `EINVAL`, dead-signature → `EBADSIGN`), except
  `mdbx_cursor_renew()` which re-binds it;
* `cursor::update_current()/reserve_current()` copy the current key into a
  `default_buffer` iff `mdbx_is_dirty()` reports the key's page dirty —
  the correct strong-guarantee idiom for `MDBX_CURRENT` updates (clean
  pages survive COW, dirty pages must be snapshotted before the in-place
  update).

### 1.4 Transcoders and `slice` — verified arithmetic

* **`write_bytes()` bounds.** For all six coders, `envisage_result_length()`
  is a true upper bound of what `write_bytes()` writes, including newline
  insertion (`bytes + bytes/wrap_width` floors dominate the actual newline
  count for every wrap width) and base58 leading-'1' dedup. Machine-checked
  exhaustively for lengths 0..200 × wrap widths {0,1,3,4,8,17,64} with
  canaries (F-V13A probe).
* **Nibble/digit arithmetic.** `to_hex`'s `(((9-hi)>>7)&alpha_offset)` and
  `from_hex`'s `(c|0x20)-'a' + 10 + ((h>>7)&39)` are branchless digit
  encodings correct over the full `isxdigit` set; base58 encode/decode use
  58⁹/58⁴ word moduli with `<<CHAR_BIT` carry — no overflow for any slice
  length representable in a DB (the `size_t` multiply in `b58_buffer`'s
  ratio scaling would need multi-exabyte slices to overflow).
* **Encode/decode word frontier.** The base58 output loop writes 9 chars per
  8-byte word while reading words top-down; the sizing formula
  `words = 8·ceil(ceil(len·11/8)/7)` keeps the byte-write pointer strictly
  below the word-read frontier (the internal `ASSERT(ptr < porous)` holds
  for every reachable input, checked analytically + fuzz).
* **`is_erroneous()` vs `write_bytes()` agreement** — holds for every
  adversarial input except sources with **zero decodable characters**
  (F-V13A-2).

### 1.5 Error mapping

`error::throw_exception()` maps `MDBX_EINVAL`→`std::invalid_argument`,
`MDBX_ENOMEM`→`std::bad_alloc`, `MDBX_SUCCESS/RESULT_TRUE`→`std::logic_error`
(the latter documents "must not be called with a success code"), all
`is_mdbx_error()` codes to the typed `mdbx::exception` hierarchy (`fatal`
for `EBADSIGN`/`PANIC`), and anything else to
`std::system_error(std::system_category())`. `success_or_throw` treats only
exact `MDBX_SUCCESS` as success; `boolean_or_throw` accepts only
`RESULT_FALSE`/`RESULT_TRUE`. Callback thunks (`exception_thunk`) capture
user-visitor exceptions and rethrow them **after** the C call returns —
`txn::enumerate_tables`, `txn::get_gc_info`, `env::enumerate_readers`,
`env::defrag` and the `data_preserver` used by `txn::extract/replace*`
(allocator failure inside the C `replace_ex` surfaces as the original
`bad_alloc`, not as a corrupted status). A visitor is never re-entered
after it threw (thunks check `is_clean()` and return the loop-exit code).

### 1.6 `make_flags` mode/durability folding

`operate_parameters::make_flags()` silently clears `MDBX_WRITEMAP` when
`options.nested_transactions` is requested (nested txns are file-IO only);
with `MDBX_ACCEDE` an existing WRITEMAP environment re-adds the flag and the
post-open check throws `incompatible_operation`. So: fresh create =
silent downgrade to `write_file_io`; accede onto a WRITEMAP env = explicit
exception. Durability options are folded only for non-readonly modes.
Both scoped enums (`env::mode`, `env::durability`) reject out-of-range
cast-in values via `default:` → `std::invalid_argument` (breaking-change
hardening, #144 anchor).

---

## 2. `docs/engineering/libmdbx-invariants.md` additions (C++ area)

### INV-CXX1 — managed wrappers never own a handle the C core already freed

For each wrapper, `handle_` is null at every point where the underlying C
object is freed, and non-null wherever the object is guaranteed alive:
env per §1.1 (post-signature-CAS ⇒ freed ⇒ null); txn commit/abort per the
documented "terminated unless THREAD_MISMATCH" contract; checkpoint per the
keep-set `{THREAD_MISMATCH, EINVAL, BAD_TXN}`; embark_read via in-place
`*ptxn` update; cursor never freed behind the wrapper (detached
ready4dispose instead, §1.3). Verified per-exit for all 106 functions of
`src/mdbx.c++`; the four `#97`/`#105` fixes (env close dispatch, four txn
error paths) are still in place at `src/mdbx.c++:1359-1651`.

### INV-CXX2 — constructor exception safety is delegated to the base-ctor idiom

`X(args) : X(resource()) { throwing-setup(); }` — the delegating body's
throw runs `~X()`, which releases the resource. Applies to all
`env_managed`/`txn_managed`/`cursor_managed` constructors; empirically
confirmed (C++ [except.ctor], LSan clean on 50 failed opens). Rule for
review: no throwing statement may be hoisted into a mem-initializer of a
managed wrapper's primary (non-delegating) constructor.

### INV-CXX3 — user exceptions cross the C boundary only through thunks

Every C callback trampoline in the C++ API wraps the visitor in an
`exception_thunk`: first callback invocation after a throw returns the
loop-exit code and no further visitor calls happen; the captured exception
is rethrown after the C call returns. There is no path where a user
exception unwinds through C frames (which would be UB), and no path where a
visitor throwing is silently swallowed (the rethrow precedes status
checking via `success_or_throw(rc, thunk)` / `boolean_or_throw(rc, thunk)`).

---

## 3. Findings

### F-V13A-1 — stale justification in the #92 commit message (doc hazard, low)
`4693ccca` ("refuse to commit/abort a parent with a live nested child")
claims "Like checkpoint() already does, commit()/abort() now refuse…".
`checkpoint()` never had such a guard (only 4cc8f11c + a reformat touched
it); it does not need one because the C core's `check_txn` rejects
`MDBX_TXN_HAS_CHILD` at `mdbx_txn_checkpoint()`'s entry (§1.2). Behavior is
correct; the message misleads future readers about *where* the guarantee
lives. Action: none in code; optionally note in cxx-api.md (§1.2 wording).

### F-V13A-2 — `from_hex::is_erroneous()` / `from_base64::is_erroneous()` reject sources with zero decodable characters that `write_bytes()` accepts (consistency, low)
`slice("").is_hex() == false`, `slice("").is_base64() == false`, while
`slice("").is_base58() == true` and `from_hex("").write_bytes()` succeeds
producing 0 bytes. Root cause: the `got` flag ("did we see any digit") makes
`is_erroneous()` return `!got` = true for empty/all-space (with
`ignore_spaces`) sources, while `write_bytes()` has no such requirement
(src/mdbx.c++:722-743 hex, 1132-1159 base64, no flag in base58
964-973). 1744 occurrences in the adversarial corpus, all with no decodable
character. No memory impact; predicate-only inconsistency between the three
decoders. Fix direction: drop the `got` flag, or document "empty input is
erroneous for hex/base64 but valid for base58".

### F-V13A-3 — `slice::is_printable(disable_utf8=true)` accepts bytes `0xE0–0xEF` (arithmetic flag-aliasing, low)
The ASCII-mode predicate `(P_|X_) & map[*src]` tests bits 3–4 of the packed
classification, but the 3-byte-UTF-8-lead classes are encoded `3 << LS`
(= 48, bit 4 set) which aliases `P_` (= `1 << LS` = 16). Result: bytes
`0xE0–0xEF` are reported printable in ASCII mode, although the same
function's map classifies them as UTF-8 lead bytes and the sibling lead
classes `0xC2–0xDF` (`2<<LS`=32) and `0xF0–0xF4` (`4<<LS`=64) are correctly
rejected (exhaustive single-byte check; multi-byte strings containing
0xE0-0xEF likewise pass: e.g. `"\xE4\x65"` → true). Intended set per the
map: `0x20–0x7E ∪ {82–8C, 8E, 91–9C, 9E–BF, C0, C1, F5–FF}`. Diagnostic-only
impact (`operator<<(ostream, slice)` uses the default UTF-8 mode). Fix
direction: mask the flags before the test, e.g.
`((P_|X_) & (map[b] & 0x0F | (map[b] & X_))) != 0` — or classify
`((map[b] & 0x18) == 0 && (map[b] & 0x07) == 0) || (map[b] & 0x18) != 0`
per the intended semantics; minimal diff is testing `(map[*src] ^ (map[*src] >> 1)) & 0x18`-free
variants — to be chosen by the owner.

### REFUTED hypothesis (recorded): checkpoint() child-UAF
The initial review hypothesis — `txn_managed::checkpoint()` frees a live
nested `txn_managed` child behind its wrapper (mirroring the #92 defect for
commit/abort) — was **refuted by the control experiment**: the C core's
entry `check_txn` includes `MDBX_TXN_HAS_CHILD` in `bad_bits`, the wrapper
call throws `bad_transaction` and both handles stay valid
(`v13a-checkpoint-has-child-control.cpp`, E1).

---

## 4. Per-function verdict ledger (grouped; all 106 functions)

`src/mdbx.c++` functions grouped by family; "PROVED" = statement-level
argument over every exit; "PROVED·A" = anchored additionally on a C-core
contract verified in wave 2/4 or on the machine checks above.

| family (functions) | verdict | notes |
|---|---|---|
| `throw_max_length_exceeded/throw_too_small_target_buffer/throw_out_range/throw_allocators_mismatch/throw_incomparable_cursors/throw_bad_value_size` | PROVED | trivial throw helpers, `[[noreturn]]` |
| `exception::exception/~exception` + 34 `DEFINE_EXCEPTION` ctors/dtors | PROVED | no-owned-resource; `~fatal` terminate-on-last-destruction is INV-anchored |
| `fatal::fatal/~fatal` | PROVED | counter ±1 balanced on every path (C++17 throw = no copy) |
| `error::what/message/throw_exception` | PROVED | mapping total over `is_mdbx_error()` ∪ system set ∪ default |
| `slice::is_printable` | PROVED·A(fuzz), **F-V13A-3** | UTF-8 mode exact vs reference (40k cases); ASCII mode over-accepts E0-EF |
| `slice::as_{u,i}{8,16,32,64,128}_adapt` (10) | PROVED | `memcpy` punning (no strict-aliasing UB); sizes guarded, `*_8` throws `bad_value_size` on oversize |
| `to_hex::write_bytes/output` | PROVED·A(fuzz) | bounds = envisage; branchless nibble correct |
| `from_hex::write_bytes/is_erroneous` | PROVED·A(fuzz), **F-V13A-2** | agreement holds except no-digit sources |
| `to_base58::write_bytes/output`, `b58_encode/b58_decode/b58_8to11` | PROVED·A(fuzz) | word-frontier argument §1.4; roundtrips exact incl. leading zeros |
| `from_base58::write_bytes/is_erroneous` | PROVED·A(fuzz) | no `got` asymmetry |
| `to_base64::write_bytes/output`, `b64_3to4/b64_4to3` | PROVED·A(fuzz) | padding logic `c>=0`→1 pad, `c==d==EQ`→2 pads mirrors is_erroneous |
| `from_base64::write_bytes/is_erroneous` | PROVED·A(fuzz), **F-V13A-2** | |
| `line_wrapper::put` (×2), `temp_buffer` | PROVED | bulk-vs-wrap condition exact; RAII correct |
| `trouble_location`/`bug`/`format*` (#if 0 block) | N-A | dead code, disabled |
| `mode2flags`, `make_flags/mode_from_flags/durability_from_flags`, `reclaiming_options/operate_options` ctors | PROVED | §1.6; out-of-range enums rejected |
| `env::is_pristine/is_empty` | PROVED·A | read-only stat/info, no writes |
| `env::copy` (6 overloads), `get_path`, `remove` (6 overloads) | PROVED·A | thin success_or_throw/boolean_or_throw wrappers |
| `create_env` | PROVED | throws on failure, no partial state |
| `env_managed::operator= / ~env_managed / close / setup` | PROVED·A | §1.1; BUSY keeps handle (pre-CAS), all other errors null (post-CAS free) |
| `env_managed` ctors (10 incl. Windows/fs) | PROVED | INV-CXX2 delegation; post-open nested/WRITEMAP re-check |
| `env_managed::open_for_recovery` (6) | PROVED | `result` is a complete local → dtor runs on throw |
| `cursor_managed::close/operator=/~`, `cursor::clone` | PROVED·A | §1.3, E7; `clone` assigns through C `cursor_copy` before returning |
| `txn::start_nested` (×2) | PROVED·A | env via `mdbx_txn_env` (signature-checked); failure ⇒ no handle |
| `txn_managed::operator=/~` | PROVED | self-move no-op; best-effort abort in dtor |
| `txn_managed::abort/commit` (+ 1-arg/latency wrappers, `abort_get_latency`, `commit_get_latency`) | PROVED·A | §1.1, §1.2, E2/E3/E6 |
| `txn_managed::checkpoint` (+wrappers) | PROVED·A | §1.2, E1; keep-set `{THREAD_MISMATCH, EINVAL, BAD_TXN}` |
| `txn_managed::commit_embark_read` (+wrappers) | PROVED·A | §1.1, E5; `RESULT_TRUE` (empty RW commit) throws `logic_error` with handle already null — documented sharp edge |
| `txn_managed::amend` | PROVED·A | per-path `*ptxn` write analysis §1.1; `handle_->userctx` read is pre-validation but benign (same allocation) |
| `txn::drop_map/clear_map/rename_map` (char*/slice + string fwd, 7) | PROVED | `throw_if_absent=false` swallows only `NOTFOUND|BAD_DBI`, all other codes thrown |
| `cursor::update_current/reserve_current` | PROVED | dirty-key snapshot idiom §1.3 |
| `operator<<` (slice/pair/pair_result/geometry size/geometry/operate_parameters/mode/durability/reclaiming/operate_options/create_parameters/log_level/debug_flags/error, 16) | PROVED | comma-operator trick in geometry::size is well-formed; `{1,"bytes"}` terminator guarantees loop exit; `INT64_MIN` negate is unreachable theoretical UB (diagnostics only) |
| `MDBX_INSTALL_API_TEMPLATE` instantiation block | PROVED | explicit template instantiation + MSVC warning suppression, no code paths |

---

## 5. Machine-check commands (reproduce)

```sh
make lib-static MDBX_BUILD_CXX=on          # or compile src/*.c once:
# for f in src/*.c; do gcc -c -fsanitize=address -g -O1 -fPIC -I. -Isrc \
#   -DMDBX_BUILD_CXX=0 -w "$f" -o /tmp/asan/$(basename $f .c).o; done
# (alloy.o alone suffices: it textually includes the whole C core)
g++ -std=gnu++23 -g -O1 -fsanitize=address,leak -fno-omit-frame-pointer \
  -DMDBX_BUILD_CXX=1 '-DMDBX_CONFIG_H="config-gnumake.h"' -I. -Isrc \
  docs/verification/repro/v13a-checkpoint-has-child-control.cpp \
  src/mdbx.c++ /tmp/asan/alloy.o -o /tmp/v13a-control -pthread -lrt
/tmp/v13a-control          # E1..E7 all "ok"
# same line with v13a-transcoder-fuzz.cpp → "FUZZ: all checks passed"
```

## 6. test-coverage gaps (proposed additions)

1. **No test closes a `cursor_managed` after its transaction ended** (the
   detached ready4dispose path, §1.3) — add E7 from the control experiment
   to the C++ test suite.
2. **No test exercises `txn_managed::checkpoint()` on a parent with a child**
   (the C-core BAD_TXN refusal) nor `commit_embark_read`'s
   empty-commit `RESULT_TRUE` → `std::logic_error` sharp edge.
3. **No test pins the self-move semantics** of the three managed wrappers.
4. **Transcoders lack wrap-width bounds tests** — the canary harness of the
   fuzz probe (exact `envisage_result_length()` destination) should become a
   unit test; today nothing fails if `envisage_result_length()` under-counts.
5. **`is_printable` ASCII mode has no test** — it currently over-accepts
   0xE0-0xEF (F-V13A-3) and no test would notice a fix or a regression.
6. **No LSan-based test that failed `env_managed` construction leaks nothing**
   (INV-CXX2) — cheap to add as a CI assertion against silent ctor rework.

## 7. KB keys written this wave

Stored keys (after vocabulary normalization): `bug:cxx:erroneous-empty-source`
(F-V13A-2), `bug:cxx:printable-ascii` (F-V13A-3),
`fact:verification:cxx-core` (INV-CXX1/2/3 + §1 contracts + machine-check
index), `fact:verification:cxx-checkpoint-guard` (F-V13A-1 + the REFUTED
hypothesis record; linked to the mapped C-side `fn:mdbx_txn_checkpoint`
context). C++ wrapper symbols are absent from the codebase-memory graph —
no `fn:` links for mdbx.c++ symbols (skip noted per protocol).
