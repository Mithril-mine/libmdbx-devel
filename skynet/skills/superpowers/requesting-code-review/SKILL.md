---
name: requesting-code-review
description: Use after implementing a change, before submitting it for review, to self-check the work and prepare a reviewable PR
---

# Requesting Code Review

*Adapted from obra/superpowers (MIT). A good review starts with a self-review and a clear,
reviewable PR.*

## Before asking for review (self-check)

1. **Run the appropriate verification levels** (`verification-before-completion`):
   - Level 1–2 minimum; higher levels per change area (engine internals → 4; build/amalgamation
     → 3; GC/meta/locking/TLS → 4–5).
   - Every claim in the PR description must be backed by command output.
2. **Re-read your diff** as a stranger:
   - Are names meaningful? Is the change minimal (YAGNI)?
   - Any dead code, leftover debug, TODO left behind?
   - Comments explain *why*, not *what* (Russian/English per surrounding code).
3. **Check the project-specific rules**:
   - LLVM style (`make reformat` idempotent);
   - no disk-format changes;
   - `dist-cutoff`/`DIST_EXTRA` respected for build files;
   - Windows (MSVC/MinGW) + Linux both considered;
   - new behavior covered by a fast deterministic test (or justified otherwise).

## Writing the PR

On SourceCraft (see [`../../../sourcecraft/README.md`](../../../sourcecraft/README.md)):

- Title: imperative, concise; describe what and why.
- Description: problem → approach → tests/verification (with commands) → any trade-offs.
- Link the issue(s): `AddLinkedPRs`.
- Start as a draft (`CreatePullRequest`, publish=false), then `PublishPullRequest` when ready —
  CI runs on the three Linux workflows.
- Add a review checklist in the description if the change is non-trivial.

## Review request etiquette

- Ask reviewers to focus on specific risky areas (e.g., "please scrutinize page-state logic").
- Do not rush; reviews of engine changes need time.
- Be prepared to answer "why not alternative X" — the brainstormed design doc helps here.

## Project notes (libmdbx)

- Review comments on SourceCraft are anchored to file+position and iterations
  (`CreatePullRequestComment`); resolve threads via `resolution_state`.
- Decision tool `SetDecision`: `approve` / `block` / `trust` / `abstain` — `block` signals
  must-fix issues.
- Typical review focus: `src/proto.h`-declared interfaces, `MDBX_txn`-layout touches, GC/MVCC
  invariants ([`../../../architecture.md`](../../../architecture.md) §8), test quality and
  placement (`tests/ut/` vs `tests/issues/`).