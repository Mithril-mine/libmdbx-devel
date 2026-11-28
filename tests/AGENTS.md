# tests/AGENTS.md — Test-Suite Guidelines

> **Dev-only** guidance for agents and developers working on the libmdbx test
> suite (`tests/`). The basic "test first" workflow stays in the root
> [AGENTS.md](../AGENTS.md) — this file covers the test-suite specifics.
> See also [CONTRIBUTING.md](../CONTRIBUTING.md).

## 1. Layout

- `tests/ut/<area>/` — unit tests by area (`api/`, `details/`, `cursor/`,
  `dbi/`, `txn/`, `env/`, `cxx/`, `gc/`, `issues/`, `tools/`); issue
  regressions live under `tests/ut/issues/` and are modeled on the existing
  files there. The `tests/ut/api/` directory holds the tests of the libmdbx
  API surface only: the internal-component characterizations (the
  `details_*` engine units, the on-disk format corpus, the probe units)
  belong to `tests/ut/details/` with the `ut.details` label. The gtest-based
  units run where GoogleTest is usable at the requested C++ standard (C++17+);
  the old-standard cells register them as skips, so the pnl/rkl/txl engine
  branches and the format-corpus regressions (incl. the 32-bit
  crafted_meta_v023) are not covered there until a plain-C fallback for those
  cells is decided on; the plain-C units (the probes) keep running.
- `tests/ut/env-guard.h++` — the `env_files_guard` RAII helper and the
  `env_handle_guard` / `txn_handle_guard` scoped-ownership helpers (see the
  "Test hygiene & cleanup" section below).
- [`tests/test-timeouts.cmake`](test-timeouts.cmake) — the per-test CTest
  `TIMEOUT` registry (see the "Test timeouts" section below).
- `tests/ut/tools/cleanup-tool.c++` — `te_cleanup_tool`: FIXTURES_CLEANUP
  driver for the chained smoke scenarios and the `--scan` enforcement mode.
- `tests/ut/tools/golden_runner.py` — the `ut_tools_*` golden characterization
  runner. Golden files may carry line markers for environment-dependent log
  lines: a line `...` matches zero or more consecutive actual lines, a line
  `~ TEXT` matches exactly one actual line where each `...` in TEXT stands
  for any run of characters; everything else is byte-exact. Mark the
  variance at its observed position (e.g. the tmpfs-only `in-core database`
  NOTICE), never strip such lines globally — see the runner's header.
- `tests/framework/` — the stochastic test framework (`mdbx_test`); `tests/ci/`
  — CI cells and orchestration.

## 2. Naming and structure

- When adding tests, prefer extending existing files; split large files into
  rational thematic parts consistent with the tests being added.
- Do not add redundant prefixes/suffixes to test file names. All tests must
  follow a single uniform style and form a coherent, tidy structure.
- No tautology in test or test-file names, no serial numbers, no noise —
  only a strict rational minimum and order.
- Issue regressions: `issue_ghNNNN.c++`, registered in
  `tests/ut/issues/CMakeLists.txt` (mind the gates used there:
  `if(WIN32)/else()`, `if(MDBX_PROBES)`, `if(MDBX_BUILD_TOOLS)`, `NO_PCH`,
  etc.).
- Keep the registration lists ordered and flat: unconditional tests go into
  the primary section in strict numeric order; gated tests go into the few
  dedicated `if()` blocks (`if(WIN32)/else()`, `if(MDBX_PROBES)`,
  `if(MDBX_BUILD_TOOLS)`), preserving numeric order within each. A test
  needing a compound gate nests as an inner `if()` in the most suitable
  existing block instead of opening a new top-level one. Every test file
  must have exactly one registration — after merges, check for orphans in
  both directions (files without registration and vice versa).

## 3. Tests must exercise the real code — no simulacra (MANDATORY)

A test that contains a copy of library code (a "verbatim extraction", a
duplicated function body, a snapshot of an algorithm) is not a test of the
library — it is a simulacrum testing the copy: when the real code changes or
regresses, the simulacrum keeps passing silently. Such tests are categorically
forbidden in the test suite.

- The exercised logic must be the actual library sources: through the public
  API, through the testing API (`tests/api.{h,c}` - its wrappers call the
  live linked functions), or by linking the real library objects. Direct
  `#include` of the sources into a test binary is not allowed: the single
  sanctioned consumer of `src/` headers is `tests/api.c` (see §8), and a
  compiled-in copy inevitably drifts into a simulacrum.
- Macro-seams (renaming a mocked dependency before including the real source)
  are acceptable only where source inclusion itself is still sanctioned (i.e.
  inside the testing API implementation, such as the `gc-store-retired.h`
  multi-instance pattern of `tests/api.c`), as long as the code under test
  itself is the real source.
- Independent re-implementations written from the specification (oracles,
  brute-force references, alternative algorithms used for cross-checking) are
  fine: they do not copy library code and cannot drift along with it.
- Mocks, stubs and fault injection are fine for the environment around the
  code under test (I/O, clocks, allocation failures) — not for the exercised
  logic itself.
- Analysis/probe programs that copy code for investigation (e.g. under
  docs/verification/repro/) are documentation artifacts, not tests: they must
  not be registered as tests, and their findings must be backed by suite
  tests that exercise the real code.
- Litmus: a test registered in this suite must fail when the exercised
  library code is changed or broken. If no change to the library can make
  your test fail, it is not a test of the library.

## 4. Test hygiene & cleanup — MANDATORY

Every test must remove the database files it creates on **every exit path** —
including failed `ASSERT_*`, `GTEST_SKIP()` and partial corpus runs — and must
leave nothing behind on success either. This is enforced, not encouraged.

- **gtest tests opening an environment**: declare
  `const env_files_guard guard({...file names...});` as the first statement of
  the `TEST()` body (the constructor removes leftovers of an earlier crashed
  run, the destructor deletes the files on every exit path through
  `mdbx_env_delete()`, including the `-lck` companions and directory
  environments). Scope helpers: `env_handle_guard` / `txn_handle_guard` —
  declare them **after** the file guard, so handles are closed/released before
  the files are deleted; use `release()` before a successful close/commit that
  is part of the test's own assertions.
  Examples: `tests/ut/issues/issue_gh0345.c++`, `issue_gh0056.c++`,
  `issue_gh0179.c++`, `issue_gh0312.c++`.
  Caveat: when a `TEST()` body re-runs itself as a spawned child (see
  `issue_gh0217.c++`), the child must not construct the guard — put it after
  the child-mode check.
- **Chained smoke scenarios** (`mdbx_test` with `--dont-cleanup-after`, where
  later tests consume the files of earlier ones): wire each chain with CTest
  fixtures — `FIXTURES_SETUP` on the producer, `FIXTURES_REQUIRED` on the
  consumers, and a `<chain>_cleanup` test with `FIXTURES_CLEANUP` driven by
  `te_cleanup_tool` (see `smoke-long`, `smoke-quick`, `smoke_fault`,
  `dupsort_writemap`, `uniq_nested`, `singleprocess` and the `t1` chains in
  `tests/CMakeLists.txt`).
- **Enforcement**: `te_cleanup_tool --scan` recursively walks the build tree
  and fails listing any leftover database file. It runs as the
  `test_hygiene_check` ctest test (`ctest -L hygiene`) — invoke it **after**
  the whole corpus, never in parallel with the tests. If it goes red, the
  culprit test is missing its guard or its cleanup arguments.

## 5. Root-cause fixes over suppressions — MANDATORY

When a sanitizer or an analysis tool (Valgrind/Memcheck, ASan, UBSan, static
analyzers) reports a failure **caused by our code or our tests**, the cause
must be fixed — the report must not be silenced by adding an exception to the
tool's suppression file. A suppression hides the defect from every future run
and turns the tool into a rubber stamp.

- **Acceptable**: a suppression for a defect in *foreign, unfixable-here*
  code — e.g. the `bash-self-leaks` entry for the `/usr/bin/bash` internals
  that the script-driven cells drag through the tool as traced children.
- **Acceptable**: a suppression for a *tool false positive* that our code
  cannot express otherwise — and only with a comment proving it (e.g. the
  `msync-uninit-page-holes` entry: `msync()` flushes whole mapped pages
  including the never-written gaps between nodes, which nothing reads back).
- **Unacceptable**: a suppression that masks a defect of our own — a memory
  leak, a use-after-free, an uninitialized read. Fix the leak/free/init, then
  delete the failure report. An exit-path cleanup (close the environment,
  release the handles) is a root-cause fix too; `_exit()` + a suppression is
  not.
- **A fork() child that exits without cleanup is still our code**: close the
  inherited environments and free the inherited handles before `_exit()`, or
  restructure the test so the child has nothing to leak. "The child dies
  anyway" does not make the leak disappear — it makes it permanent.

## 6. Test timeouts — MANDATORY

Every registered test must carry an explicit CTest `TIMEOUT` — roughly 3× the
test's runtime in its slowest **reference** configuration, i.e. the x86-64
Linux cells (the per-cell per-test metrics artifacts of the nightly/gate
runs are the data source; the ASan builds are usually the slowest of them).
The systematically slower setups — the ARM64/ARM runners, Darwin, the Debug
builds — are covered by the configure-time platform scale (see the
"Reference calibration + platform scale" bullet below), not by larger
manifest values. A runaway test must burn its timeout budget, not the CI
cell's.

- **gtest-family tests** (`add_gtest()`/`add_ut()`/`add_issue_test()`) and
  the `extra_*` tests (`add_extra_test()`): add a `"name=seconds"` entry to
  the [`tests/test-timeouts.cmake`](test-timeouts.cmake) manifest (kept
  sorted). The helpers resolve the timeout from the manifest automatically
  and **fail the configure step** when an entry is missing, so a new test
  cannot be registered without one. An explicit `TIMEOUT` argument to the
  helper still wins over the manifest — use it only with a good reason.
- **Directly registered tests** (plain `add_test()` in `tests/CMakeLists.txt`
  and `examples/CMakeLists.txt` — the smoke chains and their cleanups,
  `ut_tools_*`, `test_hygiene_check`, the examples): set the `TIMEOUT`
  property explicitly at the registration site.
- Values are rounded up to a 30s boundary (the floor for tests running well
  under a second); update an entry when a test or a *reference*
  configuration grows slower.
- **Reference calibration + platform scale — one scheme, no
  double-counting**: the manifest values (and the explicit registration-site
  `TIMEOUT`s) are the reference calibration only — ~3× the slowest reference
  (x86-64 Linux) runtime, rounded up to a 30s boundary. At the configure
  step every registered test's `TIMEOUT` is multiplied by
  `MDBX_TEST_TIMEOUT_SCALE` — ×2 for ARM64/ARM runners, ×2 for Darwin, ×2
  for the Debug build type, composed multiplicatively and overridable, e.g.
  `-DMDBX_TEST_TIMEOUT_SCALE=4` — and, when the valgrind/memcheck runs are
  enabled, additionally by ×42. Both factors are applied by the
  `mdbx_scale_dir_test_timeouts()` call ([cmake/utils.cmake](../cmake/utils.cmake))
  at the end of each registering CMakeLists (`tests/CMakeLists.txt`,
  `tests/ut/issues/CMakeLists.txt`, `examples/CMakeLists.txt`); repeated
  calls are idempotent, each applied factor leaves its own mark on the test.
  Never raise a manifest value (or a registration-site `TIMEOUT`) because a
  slower platform/configuration kills a test — that double-counts with the
  scale and inflates every other cell; a higher value there is justified
  only when the slowest *reference* configuration itself needs it, otherwise
  the scale (or its per-cell override) handles the slower setup. Keep the
  call in place when adding a new registering CMakeLists.
- The per-test hard timeouts back up the soft gtest deadline
  (`GTEST_RUNTIME_LIMIT`, see `tests/ci/run-cell.py`): the soft limit bounds
  how deep a gtest binary digs within a run, the hard timeout kills a
  runaway binary regardless.
- **In-test wall-clock budgets follow the same scheme**: every
  `alarm()` watchdog, spin/deadline and transaction-count cap inside a test
  is calibrated for the reference configuration and must scale by the same
  factors as the CTest timeouts — the `MDBX_TEST_TIMEOUT_SCALE` env (via
  `mdbx_tst_timeout_scale()`) and ×42 under Valgrind
  (the shared `mdbx_tst_scaled_wait_seconds()` helper of
  `tests/ut/test-env.h++` is the pattern). An unscaled in-test budget fires
  long before a scaled CTest timeout would, turning a slow-but-correct run
  into false failures (observed as the SIGALRM deaths of the durability
  children in the 2026-11-24 memcheck sweep). `usleep()` and other sleeping
  waits are wall-clock and are NOT inflated by Valgrind, but every
  stat()/transaction between them is.

## 7. Cross-platform correctness

- Never compare page-size or derived values against constants — it is
  platform-dependent (use the actual values from the environment/library).
- A test's runtime must not grow with the page size: size bulk-data
  scenarios in pages and let the element size (not the element count)
  absorb the page-size difference (see `issue_gh0319.c++`), so a
  16 KiB-page CI cell runs as fast as a 4 KiB one.
- Build and test on **at least Linux and Windows**; on Windows mind the
  narrow-vs-wide literals (`MDBX_LOCK_SUFFIX` and friends are wide in unicode
  builds) and the absence of POSIX headers (`dirent.h` and friends must stay
  behind `#ifndef _WIN32` gates).
- MSVC (C mode) rejects an empty initializer for an array of unknown size —
  `(pgno_t[]){}` is error C7757; use `{0}` or spell the size out (the
  element is not read when the count is 0).
- SourceCraft CI cubes run their steps as root, and root bypasses file
  permissions: inductions via `chmod` (e.g. forcing the lck-less mode with
  `0444`) silently degrade to a normal locked open there. Gate such
  scenarios on the effective uid (`geteuid()`) or verify the induced state
  behaviorally (see `issue_gh0317.c++`).

## 8. The testing API (`tests/api.{h,c}`)

The official name of this facility is the **testing API**: the uniform
place for the additional API needed exclusively for introspection,
testing and debugging - instead of ad-hoc hooks scattered over the
sources. The former "injection" name was rejected to avoid confusion
with fault injection (the probe-bus/MPROBE family).

- declare the entry point in `tests/api.h` with a doc comment; the header
  is included from the tail of `mdbx.h`, so every consumer of the public
  header sees the declarations in development builds;
- **naming rules**: every public function is prefixed with `mdbx_tst_`,
  every private (implementation-internal) function/type with `tst_`; the
  public mirror constants follow the same `MDBX_TST_*` pattern; the
  doc-group is `c_testing`. The same `mdbx_tst_` prefix is also carried by
  the shared test-environment utilities of `tests/ut/test-env.*`
  (`mdbx_tst_getenv/setenv/unsetenv/timeout_scale/running_on_valgrind/
  mdbx_tst_scaled_wait_seconds/...`) - do not confuse them with the `c_testing`
  API entry points;
- implement it in `tests/api.c` (`#include "../src/internals.h"`), which
  is compiled **into the library**: `src/alloy.c` includes it under
  `#if MDBX_ENABLE_TESTS` (so the implementation reaches internal
  interfaces inside the same translation unit - no link-time acrobatics),
  and the non-alloy CMake source list appends it under the same gate.
  There is nothing per-test to wire up: every consumer of `mdbx.h` sees
  the declarations;
- as this API grows, the monolithic `tests/api.c` will be split into
  per-area parts in the `tests/api/` directory;
- **no direct `#include` from `src/**` in tests**: neither tests nor the
  framework `#include` anything from `src/` - not the implementation code
  (`src/*.c`) and not the headers. The single exception is `tests/api.c`,
  the implementation of the testing API: it may `#include` **headers**
  from `src/` (`internals.h` and the per-area engine headers such as
  `pnl.h`/`rkl.h`/`txl.h`) - never `.c` files. Every other access to the
  internals goes through the testing API extensions. Three reasons, in
  short:
    1. a direct `#include` couples the test to the internal scope and
       makes the planned refactorings harder (every include site must be
       re-accounted when the internals move);
    2. it obstructs using different languages for the core and the tests
       (implementation inclusion is compile-time C-only - tests could not
       be lua/tcl/python);
    3. it forces fragmenting the code into separate include files that
       have to be included into common headers and/or handled separately
       during the amalgamation.
  (Legacy white-box units and their staged rework are tracked in issue
  #519 - no permanent exceptions: `details_scan4seq`, `details_troika_fsm`,
  `details_gc_dense_solve`, `details_gc_store_retired` (the latter via the
  `gc-store-retired.h` multi-instance pattern: gc-put.c includes it for
  the REAL instance, tests/api.c - for the instrumented one with the
  stub-renamed callees), the stage-3 issue tests `gh0017`, `gh0079`,
  `gh0081`, `gh0201` (the lck/tree layout offsets, the reader-refresh
  signature and the opaque write-lock bytes are exposed through the
  computed accessors and the locking-scheme accessor; the NO_PCH flags
  of these four are dropped) and the stage-4 algorithm selftests
  `details_pnl`/`details_rkl` (the pnl/rkl/txl engines through the thin
  forwards, the opaque instance handles and the narrow corruption
  accessors; the file-static `rkl_resize()`/`extend_solid()` (un-hidden
  as `rkl_extend_solid()`)/`txl_reserve()` are un-hidden through the
  engine headers) and the stage-5 meta-format corpus
  `meta_validate_corpus`/`crafted_meta_v023`/`crafted_meta_v039` (the
  live `meta_validate()` through the `mdbx_tst_meta_validate()`
  behavioral seam and the layout-accessor family; `details-anchors.h`
  is gone) and the stage-6 `probes`/`probes_ipc` units (the probe sites
  placed through the `MDBX_TST_MPROBE_*` fire macros, the evaluators and
  the control bus through the testing API) already use the API; the
  remaining ones - the durability corpus and the framework's
  `base.h++` - are being reworked in the stage-7.)
- the wiring is gated by `MDBX_ENABLE_TESTS`: the exact value comes from
  the CMake config (`src/config.h.in`, `#cmakedefine01`), the default for
  config-less development builds from `src/options.h` - and both snippets
  are wrapped into `dist-cutoff` markers, so the amalgamated sources have
  the macro undefined and every gated region (the tails of `mdbx.h` and
  `src/alloy.c`, the non-alloy `CMakeLists.txt` branch, also
  dist-cutoff-wrapped) compiles out: the testing API is never present in
  the distributed sources, is not part of the ABI and carries no
  stability promises;
- an implementation must be **inert unless explicitly called** and must
  not be gated by `#if` conditionals (see AGENTS.md §8 of the root
  instructions): the no-op-when-unused rule replaces compile-time cuts;
- when mirroring internal constants (zone flags, walk options, page
  types), define the public `MDBX_TST_*` value in the header and pin the
  correspondence with a `STATIC_ASSERT_MSG` in `tests/api.c` (a drift is
  a build break, not a silent test skew);
- **no copies of the implementation**: neither a test nor the testing API
  may duplicate, re-implement or clone engine code - a wrapper calls the
  live linked function, a test exercises the real implementation. A copy
  inevitably drifts from the original and turns the test into a
  simulacrum checking the clone (the litmus of §3); copying the body of
  a file-static function into a wrapper is explicitly forbidden;
- **every type declared in `tests/api.h` is pinned**: each such type
  (an opaque instance handle, a mirrored struct, a value-mirror enum, a
  forwarded function-type typedef) must be referenced by a
  `STATIC_ASSERT`/`STATIC_ASSERT_MSG` in `tests/api.c` that checks it
  against the live internal definition - the `sizeof` (equality, or
  "at least as large" for the storage handles where the internal record
  may be smaller on some targets), and for the mirrored structs also the
  `offsetof` of every field the tests read (e.g. `MDBX_tst_rkl_t` against
  `sizeof(rkl_t)`, `MDBX_tst_rkl_hole_t` against `sizeof(rkl_hole_t)` and
  the `begin`/`end` offsets). The layout knowledge stays inside the
  testing API and a drift is a build break, not a silent test skew.
  The regulation lint (`tests/ci/check-testing-api.py`) enforces the
  presence of such pins for every declared type;
- **file-static engine functions are un-hidden, never copied**: when a
  test must exercise a function that is file-static in an engine module,
  drop the `static` and declare it in the engine header as
  `MDBX_INTERNAL` - the code itself does not change. A function made
  visible outside its module carries the module prefix in its name (e.g.
  `extend_solid()` becomes `rkl_extend_solid()`). Each un-hidden function
  must be justified by a concrete branch that is unreachable otherwise
  (e.g. the `rkl_resize()` shrink-failure path is not reachable through
  `rkl_reserve()`);
- **surface design**: prefer narrow reusable accessors over
  snapshot-style functions; group tightly coupled fields into one
  accessor (e.g. the solid interval bounds are always written together)
  instead of one accessor per field; do not add per-field readers for
  fields that only one or two tests use together. A thin one-to-one
  forward of an engine function is the canonical form of a call wrapper
  (zero logic, no duplication), and every entry must be justified by an
  existing call of the reworked unit - nothing is added "in reserve";
- tests do not hijack the library logging (the `#define debug_log ...`
  seams disappear together with the engine inclusions): logging behavior
  belongs to the library, and probes/instrumentation are reached through
  generic testing-API accessors following the MPROBE no-`#if` idiom;
- prefer exposing a *behavioral* seam (run the internal engine, force a
  rare-but-legal internal condition) or a *read-only introspection* of
  internal state over exposing raw internals; keep the surface small -
  each entry must be justified by a concrete test or debugging need.

## 9. Coverage honesty: skips, gates and the CI scope

A test that never runs is not coverage, and a green cell that silently
skipped the unit is not evidence. Registered does not mean executed.

- **Know where the unit runs.** `ctest -N -L '<label>'` (or `-R '<name>'`)
  is the check that the unit is inside the scope it claims: the CI cells
  select by labels (e.g. `-L 'smoke-t1|ut\.|hygiene'` in the fast+hygiene
  profile) and by name. A new unit must be inside at least one cell's
  scope, and the PR body names that cell.
- **A gtest unit disappears where GoogleTest is unusable.**
  `add_gtest()`/`add_ut()` register a *skipping stub* when GoogleTest
  requires a newer C++ standard than the cell requests (the C++11/C++14
  cells), and an extra test registered under `if(TARGET <gtest-target>)` is
  not created at all. So a characterization whose coverage must hold on the
  old-standard cells stays plain-C (`add_extra_test`) or gets a plain-C
  twin; a conversion to gtest that trades that coverage away is stated in
  the PR body and here (§12), if permanent.
- **A gate is a coverage hole by design**: `if(WIN32)`, `if(MDBX_PROBES)`,
  `if(MDBX_ENABLE_BIGFOOT)`, a `DISABLED` property — name the configuration
  that covers the case instead (e.g. the probes units are `DISABLED` on
  Windows and covered by the Linux cells), otherwise the branch is simply
  untested.
- **A skip must explain itself**: `GTEST_SKIP()` with the reason and the
  condition; a silent `return` on an unmet precondition is forbidden.
- **Green is not evidence by itself**: a new test must be shown to fail on
  the unfixed code (red before green), and the PR body says which leg
  proves it — a 64-bit host proves the 64-bit leg only, the 32-bit leg is
  proven by the win32/armeabi cells.
- **Labels are contract, not decoration**: every test carries the root `ut`
  label plus its area label (`ut.api`, `ut.cursor`, `ut.dbi`, `ut.txn`,
  `ut.env`, `ut.cxx`, `ut.gc`, `ut.issues`, `ut.tools`); the orthogonal
  labels (`ut.gc`, `ut.heavy`) are added, never substituted. The label tree
  is the comment block in `tests/CMakeLists.txt` and the source of truth: a
  new area gets its `ut.<area>` label there and in §1 in the same commit.

## 10. Determinism, ordering and signal robustness

- **Order independence.** CI runs the gtest units with `GTEST_SHUFFLE=1`
  (nightly profiles) and every corpus with `--schedule-random`, so no
  `TEST()` may depend on another `TEST()`'s state or on the declaration
  order: reset the unit's globals at the start of every `TEST()` (a small
  `tst_reset()` helper that zeroes the counters and the seed), build the
  fixture inside the `TEST()` that needs it, and never seed a PRNG in one
  `TEST()` for another one to use — the printed seed must describe the
  sequence that `TEST()` actually consumed.
- **Reproducibility.** A randomised `TEST()` prints its seed, accepts an
  override (`MDBX_RKL_CASES`/`MDBX_RKL_LOOPS`-style knobs stay documented
  in the `TEST()`), and keeps its default scale equal to the historical one
  so the nightly coverage does not quietly shrink.
- **Counter-driven verdicts stay honest.** A check vocabulary that keeps
  counters (bailouts, per-pass verdicts) reports each failure through
  `ADD_FAILURE()`/`EXPECT_*` as well, so a failed check marks the
  `TEST()`; a helper that returns early must increment its counter or flag
  the failure before returning, otherwise the `return` turns a failure into
  a pass.
- **Blocking calls are retried on `EINTR`.** `semop()`, `waitpid()`,
  `poll()`, `nanosleep()` and friends are interrupted by the suite's own
  process-directed signals (SIGCHLD, SIGALRM, the SIGUSR progress canary);
  a barrier/latch is a safe re-entry by definition, a `waitpid()` for a
  specific child restarts. Failing on `EINTR` kills a healthy cell — that
  is what the fork-storm barrier and the fork choreography used to do.
- **Wall-clock budgets scale** (§6): an `alarm()`, a deadline or a spin cap
  inside a `TEST()` goes through `mdbx_tst_timeout_scale()`, otherwise a
  slow-but-correct run turns into a false failure.
- **The soft runtime limit is a skip, not a kill**: a long `TEST()` checks
  `MDBX_GTEST_IS_EXECUTION_TIMEOUT()` at its top and skips with the reason
  (`GTEST_RUNTIME_LIMIT` bounds the dig depth, the CTest `TIMEOUT` kills a
  real runaway).

## 11. Word width, alignment and crafted fixtures

- **Host-width legs are runtime, not `#if`.** A case that exists only on a
  32-bit (or only on a 64-bit) host is gated by a runtime
  `sizeof(size_t)`/`MDBX_WORDBITS` check mapped to `GTEST_SKIP` or to the
  other branch, so one source covers both legs and the win32/armeabi-v7a
  cells exercise the narrow one.
- **A host-width regression proves both legs**: the value is preserved
  where it fits and the case is refused cleanly where it does not. Assert
  the *exact* expected code (`EXPECT_EQ(MDBX_INCOMPATIBLE, rc)`), never
  "not success"/"some error" — a change that starts refusing for the wrong
  reason must break the test.
- **Crafted page images are 8-byte aligned** by construction
  (`union align8_u { uint64_t force_align; uint8_t bytes[MDBX_MAX_PAGESIZE]; }`):
  the engine casts the buffer to `page_t *` and peeks `uint64_t` fields
  through it, and a plain array is not guaranteed to be aligned.
- **The on-disk-format corpus** builds every page through the testing API
  (never a hand-written byte layout), keeps one case per reject branch
  listed in the file header, and self-checks the crafting constants against
  the live encoders (`pages2pv()` etc.) instead of hard-coding a constant
  with a duplicated rationale. It is format-dependent by nature: a format
  change revisits it in the same PR.
- **The baseline is real**: the corpus snapshots a genuinely committed meta
  page of a freshly created database as its positive control, so a
  silently broken crafting path fails the control case first.

## 12. Converting a legacy unit to the gtest family

The staged rework converts plain-C characterizations into `add_ut()` units
(and moves them to their area); a conversion must not lose a check or a
case.

- **Fidelity is reviewable**: the number and the order of the check
  call-sites and their expected values are preserved, the case names map
  one-to-one, and the counter/bailout semantics survive (§10). Put the
  mapping in the commit message (`CHECK_TRUE` → `EXPECT_TRUE`, the counters
  kept, the bailout threshold unchanged) and diff the check sequences, not
  only the files.
- **The old `main()` disappears**: the setup becomes a helper called by
  each `TEST()`, the hygiene guards (§4) cover every `TEST()` that opens an
  environment, and the `debug_log` hijack and the `src/` inclusions go away
  in favour of the testing API (§8).
- **Registration follows the target family**: `add_ut()` plus the manifest
  entry under the CTest name, and the caller sets the labels with
  `set_tests_properties()` after the helper (the helper's own defaults are
  only the root label).
- **The coverage consequence is stated**: where the unit used to run (the
  old-standard cells) it now skips (§9) — say it in the PR body, or keep a
  plain-C twin.

## 13. Documentation map (testing)

- [docs/engineering/testing-codex.md](../docs/engineering/testing-codex.md),
  [docs/engineering/testing-methodology.md](../docs/engineering/testing-methodology.md)
  (rules, methodology, the coverage map and its white spots),
  [docs/engineering/test-scenarios.md](../docs/engineering/test-scenarios.md) +
  [test-scenarios-codex.md](../docs/engineering/test-scenarios-codex.md) —
  scenario specs and their codex.
- [docs/engineering/test-durability-strategy.md](../docs/engineering/test-durability-strategy.md) —
  durability modes and speed-up transports for tests.
- [docs/engineering/testing-infra-design.md](../docs/engineering/testing-infra-design.md) —
  CI v2 infrastructure design (background).
- [docs/engineering/testing-infra-v3.md](../docs/engineering/testing-infra-v3.md) —
  CI v3 infrastructure (implemented): registry, addressing, orchestrator, profiles.
- [docs/engineering/ci-orchestrator-requirements.md](../docs/engineering/ci-orchestrator-requirements.md) —
  authoritative requirements & design of the ci-dispatch orchestrator
  (ORCH-*/INV-*); read before changing `tests/ci/orchestrate.py` or
  `ci-dispatch.yml`.
- [docs/engineering/probe-bus.md](../docs/engineering/probe-bus.md) — probe
  conventions.

## 14. Pre-publish checklist (tests)

- the unit fails on the unfixed code and passes on the fixed one, and both
  host-width legs are proven (§9, §11);
- real library code, no simulacra, no `src/` inclusion outside the testing
  API (§3, §8);
- hygiene guards on every `TEST()` that creates files; `test_hygiene_check`
  green afterwards (§4);
- the manifest entry is present and the list stays sorted; in-test budgets
  are scaled (§6);
- the area/label placement is correct and `ctest -N -L '<label>'` shows the
  unit in the intended scope (§9);
- order-independent under `GTEST_SHUFFLE=1`, seeds printed and overridable
  (§10);
- `EINTR` retries on the blocking calls, `GTEST_SKIP()` with reasons (§10);
- the registration lists are free of orphans in both directions, and every
  moved path is updated in the comments, docs and lint pins (§2, §12);
- `ctest -R '<name>'`/`-L '<label>'` run locally on the exact final tree,
  and nothing is left behind in the build directory (§4).
