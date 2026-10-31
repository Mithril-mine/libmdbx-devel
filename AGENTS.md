# AGENTS.md — Agent Guidelines

> **Dev-only** guidance for AI agents and developers working in this
> repository. This file is part of the development tree and is **not**
> included in `make dist` artifacts. See also:
> [CONTRIBUTING.md](CONTRIBUTING.md), [docs/engineering/README.md](docs/engineering/README.md)
> (documentation index), [skynet/knowledge-map.md](skynet/knowledge-map.md)
> (project knowledge base).

## 1. Repository layout pointers

Do not duplicate the structure descriptions here — they are maintained in:

- `src/` — the library sources; `mdbx++/` — the C++ API layer
  (`decl-*.h++` / `impl-*.h++` after the rename; see
  [docs/engineering/structure.md](docs/engineering/structure.md) and
  [docs/engineering/cxx-api.md](docs/engineering/cxx-api.md)).
- `tests/` — unit tests under `tests/ut/<area>/`, issue regressions under
  `tests/ut/issues/`, CI cells under `tests/ci/`.
- `docs/engineering/` — engineering documentation (canonical codexes:
  workflows, changelog, issues/PR, testing, coverage).
- `skynet/` — agent infrastructure; `skynet/MCP-shared-graph-memory/` is the
  shared memory module (Store, scan/coverage tools, [SKILL.md](skynet/MCP-shared-graph-memory/SKILL.md),
  [docs/REFACTORING-MAP.md](skynet/MCP-shared-graph-memory/docs/REFACTORING-MAP.md));
  [skynet/knowledge-map.md](skynet/knowledge-map.md) is the knowledge index.
- `skills/{profiles,shared,superpowers}/` — canonical agent skills;
  `.opencode/skills/` holds active copies (sync rules in its README).

## 2. File and test naming

- Use `-` (not `_`) in C/C++ source file names and directory names; use
  `.c++` / `.h++` extensions for C++ sources.
- Use `_` (not `-`) in Python script names.
- When adding tests, prefer extending existing files; split large files into
  rational thematic parts consistent with the tests being added.
- Do not add redundant prefixes/suffixes to test file names. All tests must
  follow a single uniform style and form a coherent, tidy structure.
- No tautology in test or test-file names, no serial numbers, no noise —
  only a strict rational minimum and order.
- Do not keep stale or duplicating documentation files; outdated content is
  merged into the canonical file and the duplicate is removed.

## 3. Git history

Canonical policy: [skynet/git-history-policy.md](skynet/git-history-policy.md).
Summary:

- Every commit must be working and CI-green, so `git bisect` never follows
  false paths. No temporary commits, no drafts, no work-in-progress
  snapshots — only final, working code.
- Fixes are folded into the originating commit (amend / fixup + autosquash),
  not stacked on top as separate "fix" commits. Test additions only extend
  existing tests; corrections are squashed.
- Do not rewrite across git tags; ask the owner before rewriting deeper
  than a few recent commits.

## 4. General workflow

- Work by git-flow: branch → PR → review/CI. Clean the history before
  merging: fold fix commits into their sources, and resolve **all**
  reviewer/bot comments before merging.
- Merging: prefer rebase/fast-forward; a merge commit is acceptable only
  when the PR carries several independent, non-trivial changes.
- A PR must reference every related item/issue; when it fully fixes an
  issue, include a GitHub-recognized closing signature (e.g. `Closes #N`).
- Comments posted to issues/PRs start with the line `// skynet here`.
  Comment on related issues, close fixed ones, answer comments, and thank
  contributors for clarifications.

## 5. Issues workflow

- When taking an issue, assign it to yourself.
- Test first: if the issue already has a reproduction scenario, add a test
  modeled on `tests/ut/issues/**` — it must fail.
- Then implement the fix; the test must pass afterwards.
- The fix includes an adequate ChangeLog entry in a user-facing form —
  see `docs/engineering/changelog-codex.md` and section 6 below.
- External reporters are thanked in the ChangeLog `Appreciations` section
  (follow existing entries via `grep Appreciations ChangeLog.md`); never
  mention a user more than once — merge mentions logically.
- External communications rules: [docs/engineering/issues-pr-codex.md](docs/engineering/issues-pr-codex.md).

## 6. ChangeLog rules

Canonical style guide: [docs/engineering/changelog-codex.md](docs/engineering/changelog-codex.md).

- Only user-affecting changes (reaching the amalgamated build) go into
  `ChangeLog.md`; anything else only in exceptional cases, agreed with the
  owner.
- User-facing wording: no internal jargon, no low-level details — describe
  what the user would have observed.
- Prefer a single line up to 100 characters; 2–3 sentences or a short
  structural note in rare cases. Every entry must be semantically complete
  and end with a period.
- Backports get ` (backport)` immediately before the final period.
- Issues must be referenced only as markdown links (GitHub, SourceCraft or
  third-party); bare `#N` mentions are noise and must not appear anywhere,
  including commits and code.
- Internal identifiers, field names and abbreviations are not acceptable
  (rare exceptions allowed).
- Place a new entry in the appropriate existing section; propose a new
  section when nothing fits, and ask when in doubt.

## 7. Code style

- LLVM code style, **2-space indent**, `ColumnLimit: 120` (see
  [.clang-format](.clang-format) in the repository root; run it before
  publishing).
- Minimize preprocessor conditionals: do not introduce new
  `#if`/`#else`/`#endif`; migrate existing ones into system-dependent
  modules (`src/osal.h`, platform files) gradually. Prefer fallback
  `#define`s resolving to no-op functions/macros and runtime
  `MDBX_OPTION_*`-style branches over compile-time cuts (e.g. the
  `MPROBE_*` probes are no-op outside `MDBX_PROBES` builds without any
  `#if` at the call site).
- Probes/instrumentation are welcome, but never gated by `#if`.
- Cross-platform correctness, including tests: never compare page-size or
  derived values against constants — it is platform-dependent.

## 8. Build & CI hygiene

- Build with `ninja` and `ccache`; run tests in parallel (`-j 4`).
- Do not trigger unnecessary CI iterations; do not wait for red/failing
  runs — go fix them; cancel obsolete CI runs after merges.
- Promptly remove temporary directories and other debris; monitor free
  disk space before and after test runs.

## 9. Documentation map

- [docs/engineering/README.md](docs/engineering/README.md) — documentation index.
- [docs/engineering/workflows.md](docs/engineering/workflows.md) — branching,
  PR/review flow, CI triggers.
- [skynet/git-history-policy.md](skynet/git-history-policy.md) — history
  rewrite policy.
- [docs/engineering/changelog-codex.md](docs/engineering/changelog-codex.md),
  [docs/engineering/issues-pr-codex.md](docs/engineering/issues-pr-codex.md),
  [docs/engineering/news-codex.md](docs/engineering/news-codex.md) — comms and
  ChangeLog codexes.
- [docs/engineering/testing-codex.md](docs/engineering/testing-codex.md),
  [docs/engineering/testing-methodology.md](docs/engineering/testing-methodology.md),
  [docs/engineering/test-scenarios.md](docs/engineering/test-scenarios.md) +
  [test-scenarios-codex.md](docs/engineering/test-scenarios-codex.md),
  [docs/engineering/test-coverage.md](docs/engineering/test-coverage.md) —
  testing rules, scenario specs, coverage map (incl. white spots).
- [docs/engineering/probe-bus.md](docs/engineering/probe-bus.md) — probe
  conventions.
- [docs/engineering/libmdbx-invariants.md](docs/engineering/libmdbx-invariants.md),
  [docs/engineering/module-lock-matrix.md](docs/engineering/module-lock-matrix.md) —
  mandatory context for review profiles.
- [skynet/MCP-shared-graph-memory/SKILL.md](skynet/MCP-shared-graph-memory/SKILL.md),
  [skynet/MCP-shared-graph-memory/docs/REFACTORING-MAP.md](skynet/MCP-shared-graph-memory/docs/REFACTORING-MAP.md),
  [skynet/MCP-shared-graph-memory/tools/scan_all.py](skynet/MCP-shared-graph-memory/tools/scan_all.py) —
  structural map, multi-platform scanning and coverage pipeline.
- [skynet/knowledge-map.md](skynet/knowledge-map.md) — knowledge index and
  memory procedures.
