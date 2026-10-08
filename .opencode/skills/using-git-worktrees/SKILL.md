---
name: using-git-worktrees
description: Use when starting implementation work, to isolate changes in a separate worktree/branch and keep the main checkout clean
---

# Using Git Worktrees

*Adapted from obra/superpowers (MIT). Isolate each piece of work in its own worktree/branch so
experiments and multiple tasks never pollute each other.*

## Why

- Keep `master`/main checkout clean and always buildable.
- Run several independent tasks in parallel without context switching cost.
- Trivial to discard a failed experiment (`git worktree remove --force`).

## Setup

```bash
# from the main checkout
git worktree add ../libmdbx-wt-<task> -b feature/<task>
cd ../libmdbx-wt-<task>
```

Then establish the green baseline in the worktree:

```bash
make smoke && ctest --output-on-failure   # level 1-2 pulse
```

## Discipline

1. One task per worktree; branch name = task name.
2. Commit small, verifiable steps (see `writing-plans` / `executing-plans`); commit message
   references the plan/issue.
3. Keep the worktree synced with `master` (`git fetch && git merge origin/master` or rebase)
   to avoid stale-base surprises, especially before publishing a PR.
4. Never build the stochastic suite in more than one worktree at once — tests use
   `/dev/shm/mdbx-test.*` / `tmp.db` paths and heavy CPU; serialize heavy runs.
5. Sanitizer builds write into the worktree build dirs — keep them out of shared paths.

## Finishing

- Push the branch, open a draft PR (SourceCraft: `CreatePullRequest`), iterate, then
  `finishing-a-development-branch` decides merge vs keep vs discard.
- After merge: `git worktree remove <wt>` and delete the branch.

## Project notes (libmdbx)

- `GNUmakefile` targets use `TEST_DB`/`TEST_LOG` under `/dev/shm` (or `/tmp`) — two concurrent
  `make smoke`/`test-stochastic` runs in different worktrees can collide on those paths;
  override with `TEST_DB=... TEST_LOG=...` when needed.
- `@buildflags.tag`, `config-gnumake.h`, `src/version.c` are generated in the worktree tree —
  they are gitignored build artifacts; do not commit them.
- `make check` builds `dist/` and `@check-install` inside the worktree — safe, but takes time
  (level 3).