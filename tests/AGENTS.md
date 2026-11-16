# tests/AGENTS.md — Test-Suite Guidelines

> **Dev-only** guidance for agents and developers working on the libmdbx test
> suite (`tests/`). The basic "test first" workflow stays in the root
> [AGENTS.md](../AGENTS.md) — this file covers the test-suite specifics.
> See also [CONTRIBUTING.md](../CONTRIBUTING.md).

## 1. Layout

- `tests/ut/<area>/` — unit tests by area (`api/`, `cursor/`, `dbi/`, `txn/`,
  `env/`, `cxx/`, `gc/`, `issues/`, `tools/`); issue regressions live under
  `tests/ut/issues/` and are modeled on the existing files there.
- `tests/ut/env-guard.h++` — the `env_files_guard` RAII helper and the
  `env_handle_guard` / `txn_handle_guard` scoped-ownership helpers (see the
  "Test hygiene & cleanup" section below).
- [`tests/test-timeouts.cmake`](test-timeouts.cmake) — the per-test CTest
  `TIMEOUT` registry (see the "Test timeouts" section below).
- `tests/ut/tools/cleanup-tool.c` — `te_cleanup_tool`: FIXTURES_CLEANUP driver
  for the chained smoke scenarios and the `--scan` enforcement mode.
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
  API, or by compiling the real source into the test binary (the
  `details_pnl.c` style: `#include "../../../src/pnl.c"`), or by linking the
  real library objects.
- Macro-seams (renaming a mocked dependency before including the real source)
  are acceptable, as long as the code under test itself is the real source.
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
test's runtime in its **slowest configuration** across the CI cells (the
per-cell per-test metrics artifacts of the nightly/gate runs are the data
source; the ASan builds and the Windows/ARM64 runners are usually the
slowest). A runaway test must burn its timeout budget, not the CI cell's.

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
  under a second); update an entry when a test or a configuration grows
  slower. The valgrind/memcheck runs scale timeouts automatically (×42).
- The per-test hard timeouts back up the soft gtest deadline
  (`GTEST_RUNTIME_LIMIT`, see `tests/ci/run-cell.py`): the soft limit bounds
  how deep a gtest binary digs within a run, the hard timeout kills a
  runaway binary regardless.

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
  doc-group is `c_testing`;
- implement it in `tests/api.c` (`#include "../src/internals.h"`), which
  is compiled **into the library**: `src/alloy.c` includes it under
  `#if MDBX_ENABLE_TESTS` (so the implementation reaches internal
  interfaces inside the same translation unit - no link-time acrobatics),
  and the non-alloy CMake source list appends it under the same gate.
  There is nothing per-test to wire up: every consumer of `mdbx.h` sees
  the declarations;
- as this API grows, the monolithic `tests/api.c` will be split into
  per-area parts in the `tests/api/` directory;
- **no direct `#include` of the implementation**: tests must not
  `#include` library implementation code (`src/*.c`) or implementation
  headers (`internals.h`) to reach internals - such access goes through
  the testing API extensions. Three reasons, in short:
    1. a direct `#include` couples the test to the internal scope and
       makes the planned refactorings harder (every include site must be
       re-accounted when the internals move);
    2. it obstructs using different languages for the core and the tests
       (implementation inclusion is compile-time C-only - tests could not
       be lua/tcl/python);
    3. it forces fragmenting the code into separate include files that
       have to be included into common headers and/or handled separately
       during the amalgamation.
  (Legacy white-box units - `details_rkl/pnl/scan4seq`, the
  `meta_validate_corpus` format corpus, and the `details_gc_store_retired`
  hook-replacement GC state unit - are registered exceptions; their staged
  rework is tracked in issue #519. `details_gc_dense_solve` and
  `details_troika_fsm` already use the API.)
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
  not be gated by `#if` conditionals (see AGENTS.md §7 of the root
  instructions): the no-op-when-unused rule replaces compile-time cuts;
- when mirroring internal constants (zone flags, walk options, page
  types), define the public `MDBX_TST_*` value in the header and pin the
  correspondence with a `STATIC_ASSERT_MSG` in `tests/api.c` (a drift is
  a build break, not a silent test skew);
- prefer exposing a *behavioral* seam (run the internal engine, force a
  rare-but-legal internal condition) or a *read-only introspection* of
  internal state over exposing raw internals; keep the surface small -
  each entry must be justified by a concrete test or debugging need.

## 9. Documentation map (testing)

- [docs/engineering/testing-codex.md](../docs/engineering/testing-codex.md),
  [docs/engineering/testing-methodology.md](../docs/engineering/testing-methodology.md),
  [docs/engineering/test-scenarios.md](../docs/engineering/test-scenarios.md) +
  [test-scenarios-codex.md](../docs/engineering/test-scenarios-codex.md),
  [docs/engineering/test-coverage.md](../docs/engineering/test-coverage.md) —
  testing rules, scenario specs, coverage map (incl. white spots).
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
