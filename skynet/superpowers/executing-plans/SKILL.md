---
name: executing-plans
description: Use when a plan exists, to execute it task-by-task in batches with human checkpoints, keeping the codebase green at each step
---

# Executing Plans

*Adapted from obra/superpowers (MIT). Executes an approved `writing-plans` output in small
batches, verifying each task before moving on.*

## Purpose

Turn the plan into merged, verified changes without losing the thread or letting the tree break.

## Workflow

### 1. Prepare

- Read the plan (and design doc if present).
- Set up an isolated branch/worktree (see `using-git-worktrees`).
- Establish a **green baseline**: `make smoke` + `ctest --output-on-failure` must pass before
  touching anything.

### 2. Execute a batch

- Take the next small batch of tasks (typically 1–3) from the plan.
- For each: implement → run its verification step (from the plan) → update progress.

### 3. Checkpoint with the human

After each batch, summarize:
- what changed (files),
- verification results (commands + outcomes),
- any deviations from the plan (and why).

Pause for approval before the next batch. Do not plow through the whole plan silently.

### 4. Keep it green

- After every batch: `make smoke`, `ctest`, plus sanitizer targets when touching `src/`
  (`make test-asan`, `make test-ubsan`).
- Stochastic suite (`make test-stochastic`) before declaring large engine changes done.

## Rules

1. **One concern per commit**; commit messages describe the task, referencing the plan.
2. **Never skip verification** — a task without a passing check is unfinished.
3. **Update the plan** as reality diverges; note deviations in the checkpoint message.
4. **Stop on red** — if a test fails, fix it before continuing (use `systematic-debugging`).

## Project notes (libmdbx)

- Fast feedback loops: `make -j mdbx_test mdbx_chk` to build test tooling;
  `ctest -R <name>` for a single regression; `make smoke` for the overall pulse.
- For engine changes run sanitizer builds — the CI uses `MDBX_CHECKING=2` +
  `MDBX_FORCE_ASSERTIONS=1`, match that locally to surface invariant violations.
- Beware the stochastic nature: a failure in `test-stochastic` may need `--prng-seed` capture
  (the script seeds from `date+%s+RANDOM`; see [`../build.md`](../build.md) §6.2) for
  reproduction.