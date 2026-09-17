---
name: finishing-a-development-branch
description: Use when a development branch/worktree has completed its work, to decide merge vs keep vs discard and clean up
---

# Finishing a Development Branch

*Adapted from obra/superpowers (MIT). When the plan is executed and reviewed, close the loop
deliberately.*

## Before deciding

1. **Green state required**:
   - Level 2 (`ctest`) green; `make smoke` green.
   - Engine changes: sanitizer levels (4) run; stochastic (6) run at least once per PR.
   - `make check` (level 3) if build files/headers changed.
2. **Review resolved**: all threads addressed or explicitly deferred with the human; decision
   `approve` (or maintainer's merge approval) on SourceCraft.
3. **CI green**: the three SourceCraft workflows passed for the PR.

## Decision options

| Option | When | Actions |
| --- | --- | --- |
| **Merge** | Work complete, review + CI green | `MergePullRequest` (choose squash/rebase per project convention; `delete_branch`), then remove the worktree, update changelog/issue status (`UpdateIssue` → closed), link PR to issue if not yet (`AddLinkedPRs`) |
| **Keep as PR** | Needs more iterations / human decision later | Leave draft/open; summarize open questions in the PR description |
| **Discard** | Experiment failed or superseded | `DiscardPullRequest`, `git worktree remove --force`, delete branch |

## Cleanup checklist

- [ ] Worktree removed (`git worktree remove`); stale worktrees listed via `git worktree list`
- [ ] Branch deleted locally and on remote after merge
- [ ] Issue linked + status updated (closed/inProgress per outcome)
- [ ] Changelog entry added (project keeps per-version `ChangeLog*.md`)
- [ ] Any docs touched by the change updated (including `skynet/` index if structure/build
      changed)

## Project notes (libmdbx)

- Merge on SourceCraft can be squash or rebase (`MergePullRequest` body flags); follow the
  maintainer's convention.
- After merging engine changes, watch the **daily CI cron** (03:42 UTC) for stochastic fallout;
  a post-merge regression should be reported as a new issue immediately.
- The amalgamated `dist/` is regenerated at release time, not per merge — do not commit
  generated `dist/` artifacts.