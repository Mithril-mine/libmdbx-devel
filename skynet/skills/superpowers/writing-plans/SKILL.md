---
name: writing-plans
description: Use after design approval, to turn a design into a detailed implementation plan with small, verifiable tasks
---

# Writing Plans

*Adapted from obra/superpowers (MIT). Turns an approved design into a checklist of
bite-sized engineering tasks, each with exact file paths, expected changes and verification
steps — clear enough for a fresh agent to execute without design decisions.*

## Purpose

Produce the plan that `executing-plans` (or the platform's todo list) will drive. The plan is
the contract between design and implementation.

## Rules

1. **Small tasks**: each task = 2–5 minutes of focused change (one file, one concern).
2. **Exact locations**: every task names concrete files and, where useful, functions/line areas
   (use [`../../../structure.md`](../../../structure.md) module map and `src/proto.h`).
3. **Verification per task**: each task lists how to prove it works (build, specific test,
   sanitizer, `mdbx_chk`).
4. **Order matters**: dependencies first (foundation → feature → tests → docs → cleanup).
5. **No design drift**: the plan implements the brainstormed design; note deviations explicitly.

## Plan structure

```markdown
# Plan: <title>

## Context
(1-3 sentences: goal, link to design doc)

## Tasks
- [ ] 1. <title> — <file(s)> — <what changes> — <verify by>
- [ ] 2. ...
...

## Verification (whole)
- <commands proving the complete change: make smoke, ctest, test-asan, stochastic.sh ...>
```

## Task granularity examples (libmdbx)

| Task type | Example |
| --- | --- |
| Add a test | `tests/issues/issue_ghNNNN.c++` reproducing the bug → `ctest -R issue_ghNNNN` |
| Fix a function | `src/page-ops.c` `page_split()` edge case → `make test-asan && ctest` |
| Add an option | `src/options.h` + `CMakeLists.txt` + `conanfile.py` + `GNUmakefile` → build + `mdbx_chk -V` shows it |
| Test infra | new `tests/ut/foo.c++` + registration in `tests/CMakeLists.txt` (inside `add_extra_test`) → `ctest -R foo` |

## Common pitfalls

- Planning a refactor that touches the **disk format** — prohibited (`MDBX_DATA_VERSION` is
  frozen; see architecture invariants).
- Forgetting amalgamation: any build-file change must stay inside `dist-cutoff` regions or be
  reflected in `DIST_SRC`/`DIST_EXTRA` (see [`../../../build.md`](../../../build.md) §7).
- Tasks without verification: every task must end with a runnable check.

## Project notes (libmdbx)

- Baseline checks for any engine change: `make smoke`, `make test-asan`, `make test-ubsan`,
  `ctest --output-on-failure`, and for stochastic coverage `make test-stochastic`
  (see [`../../../build.md`](../../../build.md) §6).
- Keep tests in the right bucket: deterministic regressions → `tests/ut/` or `tests/issues/`;
  behavior sweeps → `mdbx_test` scenarios (via `stochastic.sh` caseset).
- The plan file itself may live in `skynet/plans/` or be tracked via the platform todo list.