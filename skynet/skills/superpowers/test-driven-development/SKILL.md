---
name: test-driven-development
description: Use when implementing any feature or bugfix in libmdbx, before writing implementation code
---

# Test-Driven Development (TDD)

*Adapted from obra/superpowers (MIT). This project has a strong testing culture: fixes must be
proven by a failing test first, then verified with the deterministic and stochastic suites.*

## Overview

Write the test first. Watch it fail. Write minimal code to pass.

**Core principle:** If you didn't watch the test fail, you don't know if it tests the right thing.

**Violating the letter of the rules is violating the spirit of the rules.**

## When to use

**Always:**
- New features (engine, API, tools)
- Bug fixes
- Refactoring
- Behavior changes

**Exceptions (ask your human partner):**
- Throwaway prototypes
- Generated code
- Configuration files

Thinking "skip TDD just this once"? Stop. That's rationalization.

## The Iron Law

```
NO PRODUCTION CODE WITHOUT A FAILING TEST FIRST
```

Write code before the test? Delete it. Start over.

## Red-Green-Refactor

### RED — write a failing test

Place it where it belongs:

| Kind of test | Location | Registration |
| --- | --- | --- |
| Unit behavior | `tests/ut/<name>.c++` (or `.c`) | `add_extra_test(<name>)` in `tests/CMakeLists.txt` |
| Issue regression | `tests/issues/issue_ghNNNN.c++` | `tests/issues/CMakeLists.txt` |
| Framework scenario | extend `tests/framework/*` + `stochastic.sh` caseset | via `mdbx_test` options |

Test quality rules (adapted):

- One behavior per test; a clear name describing the expected behavior.
- Assert on **real behavior**, not mocks. For libmdbx: use real DBs in `--pathname` dirs,
  real CRUD via the public API, then validate with `mdbx_chk`.
- Prefer testing through the public API (`mdbx.h`/`mdbx.h++`); for genuinely internal data
  structures use the white-box pattern: include the module source directly
  (e.g. `tests/ut/details_rkl.c` does `#include "../../src/rkl.c"` + `txl.c`), or include an
  internal header (like `tests/issues/issue_gh0017.c` uses `essentials.h` for the ABI-layout
  check). See `skynet/test-coverage.md` for the coverage map.

### Verify RED — watch it fail

Run the specific test, e.g.:

```bash
ctest --output-on-failure -R issue_ghNNNN      # CMake-built test
# or directly:
./test_extra_issue_ghNNNN                      # after make build-test
```

Confirm:
- Test **fails** (not errors).
- Failure message is the expected one (feature missing, not a typo).
- It fails for the right reason.

**Test passes immediately?** You are testing existing behavior — fix the test.
**Test errors?** Fix the error and re-run until it fails correctly.

### GREEN — minimal code

Write the simplest change that makes the test pass. No extra features, no refactoring, no
"improvements". For engine work this usually means touching exactly the module named in the
plan (e.g. `src/page-ops.c`), not restructuring.

### Verify GREEN — watch it pass

```bash
ctest --output-on-failure -R <name>
make smoke                       # regression pulse
```

Then, because the engine is intricate, run the broader gates before committing:
`make test-asan` and `make test-ubsan` (they rebuild with sanitizers + `MDBX_CHECKING=2`),
then `mdbx_chk -vvn` on any produced DB.

### REFACTOR — clean up (only after green)

Remove duplication, improve names, extract helpers. Keep tests green. Don't add behavior.

## Common rationalizations (all rejected)

| Excuse | Reality |
| --- | --- |
| "I'll test after" | Tests written after pass immediately — they never proved they can catch the bug |
| "Already manually tested" | Manual is ad-hoc and unrepeatable; the stochastic suite exists because this engine needs repeatable proof |
| "Too simple to test" | Simple code breaks; a 30-second test is cheap |
| "Existing code has no tests" | You're improving it — add tests for existing behavior |
| "Keep the code as reference" | Delete means delete; rewrite from tests |
| "TDD is slow" | TDD is the pragmatic path: regressions caught before commit are far cheaper than debugging in production |
| "This is different because..." | It never is |

## Bug fixes specifically

1. Write a failing test reproducing the bug (`tests/issues/issue_ghNNNN.c++`).
2. Watch it fail; confirm the failure matches the reported symptom.
3. Fix minimally; watch it pass.
4. Add defense: run the related test family + sanitizers + `mdbx_chk`.
5. Consider whether the bug also affects other paths (same root cause) — add coverage.

Never fix a bug without a test that proves the fix and prevents regression.

## Verification checklist (before marking complete)

- [ ] Every new/changed behavior has a test
- [ ] Watched each test fail for the expected reason before implementing
- [ ] Wrote minimal code to pass
- [ ] All related tests pass; output pristine (no new warnings)
- [ ] Sanitizer builds clean (`make test-asan`, `make test-ubsan`) for engine changes
- [ ] `mdbx_chk` validates any DB produced by the tests
- [ ] Stochastic suite run where applicable (`make test-stochastic`)

## Project notes (libmdbx)

- Deterministic regressions live in `tests/ut/` and `tests/issues/`; behavior sweeps belong in
  the `mdbx_test` framework (extend `stochastic.sh` caseset or add a scenario module in
  `tests/framework/`).
- Register new tests inside the `MDBX_BUILD_CXX`/platform guards as the existing ones do —
  keep Windows (MSVC/MinGW) and Linux both covered per `AGENTS.md`.
- Remember amalgamation: tests never ship in `dist/` (see [`../../../build.md`](../../../build.md) §7), so
  test-only code needs no cutoff markers, but build-file additions do.