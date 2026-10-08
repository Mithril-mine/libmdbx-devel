---
name: receiving-code-review
description: Use when review feedback arrives on your PR or change, to process it constructively and land better code
---

# Receiving Code Review

*Adapted from obra/superpowers (MIT). Review is a debugging and quality tool, not an attack on
your work.*

## Mindset

- The reviewer found *something* — treat each comment as a hypothesis about a real issue, even
  if the proposed fix is wrong.
- Cost of being wrong is cheap in review and expensive in production: thank reviewers for
  catches, especially on engine invariants.

## Processing comments

1. **Read all comments first**; group by theme (correctness, tests, style, docs).
2. **Classify each**:
   - Must fix (bug, invariant violation, missing test);
   - Should fix (design smell, missing edge-case coverage);
   - Can defer (style nits, future work) — but acknowledge explicitly.
3. **For "won't fix"** — respond with evidence: point to the test that covers it, or explain
   the trade-off; do not argue by authority.
4. **For valid catches** — write the failing test first, then fix (TDD), then re-run the
   appropriate verification levels from `verification-before-completion`.
5. **Resolve threads** on SourceCraft (`UpdatePullRequestComment` → `resolution_state`) after
   the fix lands in a new iteration.

## When you disagree

- Ask for the specific failing scenario ("can you give the repro/seed/mode?").
- Reproduce locally before answering; speculation is not evidence.
- Escalate to the human (maintainer) for architectural disagreements.

## After rework

- Re-run: own regression (level 1), affected family, `ctest` (level 2), plus higher levels if
  the fix touched engine internals.
- Reply in each thread: what changed and the verification command + result.
- Update the PR description if scope/approach changed.

## Project notes (libmdbx)

- Engine reviewers will care about: page-state machine, meta two-phase update, GC vs readers,
  TLS cleanup, `proto.h` API surface, disk-format stability, and whether tests are fast
  (levels 1–2) or only slow (level 6) — prefer adding fast deterministic tests per review
  feedback.