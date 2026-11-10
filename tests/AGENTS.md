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
- `tests/ut/tools/cleanup-tool.c` — `te_cleanup_tool`: FIXTURES_CLEANUP driver
  for the chained smoke scenarios and the `--scan` enforcement mode.
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

## 3. Test hygiene & cleanup — MANDATORY

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

## 4. Cross-platform correctness

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

## 5. Documentation map (testing)

- [docs/engineering/testing-codex.md](../docs/engineering/testing-codex.md),
  [docs/engineering/testing-methodology.md](../docs/engineering/testing-methodology.md),
  [docs/engineering/test-scenarios.md](../docs/engineering/test-scenarios.md) +
  [test-scenarios-codex.md](../docs/engineering/test-scenarios-codex.md),
  [docs/engineering/test-coverage.md](../docs/engineering/test-coverage.md) —
  testing rules, scenario specs, coverage map (incl. white spots).
- [docs/engineering/test-durability-strategy.md](../docs/engineering/test-durability-strategy.md) —
  durability modes and speed-up transports for tests.
- [docs/engineering/testing-infra-design.md](../docs/engineering/testing-infra-design.md) —
  CI v2 infrastructure design.
- [docs/engineering/probe-bus.md](../docs/engineering/probe-bus.md) — probe
  conventions.
