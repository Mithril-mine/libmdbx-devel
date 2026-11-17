
## Testing instructions
 - use CMake for build and CTest for testing
 - use at least Linux and Windows both environments for build and testing
 - tests must exercise the real library code: no copies of library code
   inside tests ("simulacra"); independent from-specification oracles and
   environment mocks are fine
 - cross-platform correctness: never compare page-size or derived values
   against constants — use the actual values from the environment/library

## Code style
 - use LLVM codestyle

## General workflow

- Work by git-flow: branch → PR → review/CI. Clean the history before
  merging: fold fix commits into their sources, and resolve **all**
  reviewer/bot comments before merging.
- Merging: prefer rebase/fast-forward; a merge commit is acceptable only
  when the PR carries several independent, non-trivial changes.
- A PR must reference every related item/issue; when it fully fixes an
  issue, include a GitHub-recognized closing signature (e.g. `Closes #N`).
- When taking an issue/backport task, assign it to yourself; do not take
  issues assigned to others without an explicit hand-off. Backports
  reference the original issue as a markdown link; external reporters are
  thanked in the ChangeLog `Appreciations` section (never mention a user
  more than once). Comments posted to issues/PRs start with the line
  `// skynet here`.

## Git history

- Every commit must be working and CI-green, so `git bisect` never follows
  false paths: fixes are folded into their originating commits (amend/
  fixup), not stacked on top as separate "fix the CI" commits.
- Do not rewrite history across git tags or release branches; rewriting
  deeper than a few recent commits requires the owner's approval. Before a
  force-push: create a backup branch, verify every rewritten point
  locally, and push with `--force-with-lease`.

## Work organization — worktrees, session scope, PR lifecycle (MANDATORY)

- Work only inside your own worktree/workset; never modify anything
  outside it — other agents' worktrees, foreign session directories, or
  the main checkout's working files.
- A worktree/workset you create must live **inside the session's main
  directory**, not alongside it or anywhere else.
- When the work is finished, clean up everything: remove the temporary
  worktree, delete its temporary branches, and remove any temporary files
  it produced.
- Every pull request is opened as a **draft** and moved to
  **ready-for-review** only after the work is complete and its CI is
  green.
- A `ready-for-review` PR that violates any rule of this AGENTS.md may
  be deleted by the bot together with its branch.

## Testing rules

- Tests must exercise the real library code: copies of library code inside
  a test ("simulacra", e.g. verbatim extractions) are forbidden — the
  exercised logic must be the actual library sources (public API,
  compiling the real source into the test binary, or linking the real
  objects). Independent from-specification oracles and environment mocks
  are fine.
- Cross-platform correctness: never compare page-size or derived values
  against constants — it is platform-dependent; use the actual values from
  the environment/library.

## ChangeLog rules

The format follows [Keep a Changelog 2.0.0](https://keepachangelog.com/en/2.0.0/)
with the house deviations listed here.

Scope and curation:

- Only user-affecting changes (reaching the release build) go into
  `ChangeLog.md`; curate, don't accumulate — the ChangeLog is a summary of
  notable changes for readers, not a record of commits.
- One cause — one entry: related fixes from one issue are a single entry
  listing the affected APIs, not a line per function.

Structure:

- A version section uses only the Keep-a-Changelog types: `Added`,
  `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`, plus the house
  `Appreciations` section. No other top-level sections; an empty type is
  omitted.
- Every entry begins with the verb of its type: `Added …`, `Changed …`,
  `Deprecated …`, `Removed …`, `Fixed …`; a `Security` entry begins with
  its CVE identifier when present.
- `Fixed` keeps thematic subsections; subsections exist only inside
  `Fixed`, follow the established taxonomy and stay few and stable.
- A version opens with a summary of one or two plain sentences: what the
  release gives the user.
- Breaking changes are marked `**Breaking:**` on the entry itself, within
  its type — never collected into a separate section.
- The version being prepared is the upcoming version's section with the
  stage placeholder (e.g. `at/in a release engineering stage`, optionally
  with an `LTS` prefix); a released version is `## vX.Y.Z at YYYY-MM-DD`.

Wording:

- User-facing wording: no internal jargon, no low-level details — describe
  what the user would have observed.
- An entry is a single sentence of up to 100 characters, not counting the
  trailing issue links. Longer entries only for large reworks and only
  with the owner's direction. Every entry must be semantically complete
  and end with a period.
- Backports get ` (backport)` immediately before the final period.
- Issues must be referenced only as markdown links; bare `#N` mentions
  are noise.

House deviations from Keep a Changelog: the `Appreciations` section;
thematic subsections inside `Fixed`; issue links inline at the end of
entries; the stage placeholder instead of `[Unreleased]`.
