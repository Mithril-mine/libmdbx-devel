---
name: verification-before-completion
description: Use before declaring any bugfix or feature complete, choosing the right verification level (1..7) for the change and context
---

# Verification Before Completion

*Adapted from obra/superpowers (MIT). "It works" is a claim; proof is evidence. Run the checks,
show the output, then declare completion.*

## Why

Two classic failure modes:
1. The fix works for the tested case but misses the real condition.
2. The fix works but breaks something else.

Both are caught by systematic verification with **visible evidence**.

## Context: this project has long tests

libmdbx is a complex engine and its stochastic suites are expensive. Therefore verification is
organized as **escalating levels**; pick the level justified by the change size, risk and the
stage of work (per-step / per-milestone / per-PR / release). Never jump straight to the top —
but never skip the levels your change deserves.

## Verification levels

| Level | What | Typical commands | Cost |
| --- | --- | --- | --- |
| **1** | The specific test for the change | `ctest --output-on-failure -R <your_test>` (or run the built executable directly) | seconds |
| **1-x** | Same test, but rebuilt under a sanitizer — ASAN, UBSAN or MEMCHECK, chosen by context (UB-prone code → ubsan; pointers/memory → asan; leaks/lifecycle → memcheck) | CMake build dir with `-DENABLE_ASAN=ON` / `-DENABLE_UBSAN=ON` / `-DENABLE_MEMCHECK=ON`, then `ctest -R <your_test>` | minutes |
| **2** | All deterministic CTest tests | `ctest --output-on-failure` | 3–7 min |
| **3** | `make check` — smoke + install + **amalgamation** (`dist/`) validation | `make check` | 5–10 min |
| **4** | Level 3 + sanitizer sweeps | `make test-ubsan` && `make test-asan` | 10–30 min |
| **5** | + Valgrind/memcheck sweep | `make test-memcheck` | 30–90 min |
| **6** | Stochastic run with a context-chosen option set and iteration count; if needed, the specific scenario rebuilt under ASAN/UBSAN/MEMCHECK | `tests/stochastic.sh --loops N --probe-duration ... [--pagesize ... --mode ... --extra]`; e.g. `make test-stochastic` (bounded) or targeted `stochastic.sh` invocation | 10+ min, context-dependent |
| **7** | Human-controlled extended testing | `tests/battery-tmux.sh` (tmux soak, hours/days until a human stops it), `test-long`, NUMA machines | indefinite |

## How to choose the level

| Situation | Minimum level |
| --- | --- |
| Small step during plan execution (see `executing-plans`) | 1 (add 1-x when touching memory/UB-sensitive paths) |
| Feature/bugfix milestone in `src/` | 2, plus 1-x for the touched area |
| Changes to build files, headers, or anything affecting amalgamation | 3 (validates `dist/`) |
| Engine-internal logic (GC, meta, locking, MVCC, TLS) | 4 (and 5 when lifecycle/leaks are in scope) |
| PR finalization | 4–6 depending on touched areas |
| Release / deep confidence | 6–7 (human-supervised) |

## Rules

1. **Run the checks, do not assume**: a check you didn't run proves nothing.
2. **Show evidence**: paste actual command output / exit codes in the completion message.
3. **New warnings are failures**: build output must be pristine (`-Werror` upstream).
4. **Stochastic flakiness**: if a level-6 run fails, capture the seed/mode and reproduce
   deterministically; a flaky failure is still a failure.
5. **Cross-platform**: per `AGENTS.md`, engine/build changes must be verified on Linux **and**
   Windows (CMake+CTest) before merging — treat the second platform as a level-2-equivalent
   requirement for `src/` changes.
6. **Prefer fast tests**: when the only coverage for a behavior would be a slow (level-6)
   scenario, invest in a fast unit test instead — expanding the fast deterministic suite is a
   near-term project goal.

## Completion checklist

- [ ] Level 1: own regression passes (watched RED before fix)
- [ ] Level 1-x applied where context demands (sanitizer rebuild of the specific test)
- [ ] Level 2: full `ctest` passes
- [ ] Level 3–6 run as justified by change area/risk (or explicitly scheduled with the human)
- [ ] No new compiler warnings
- [ ] Windows build/test considered (or explicitly delegated)
- [ ] `mdbx_chk` validation done where a DB was created/modified

## Project notes (libmdbx)

- CI runs with `MDBX_CHECKING=2`, `MDBX_DEBUG=0`, `MDBX_FORCE_ASSERTIONS=1`,
  `MDBX_DEBUG_SPILLING=1` — replicate those locally before relying on `make smoke`.
- Sanitizer targets rebuild with their own flags (`CFLAGS_EXTRA`, `CMAKE_OPT`); always re-run
  after editing `src/`. See [`../build.md`](../build.md) §6.4 for exact commands.
- `make check` additionally validates the amalgamated `dist/` build; run it when build files or
  headers changed (see [`../build.md`](../build.md) §7).