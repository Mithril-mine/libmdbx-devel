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
- `tests/` — the test suite; test-suite guidelines live in
  [tests/AGENTS.md](tests/AGENTS.md) (layout, naming, hygiene/cleanup rules).
- `docs/engineering/` — engineering documentation (canonical codexes:
  workflows, changelog, issues/PR, testing, coverage).
- `skynet/` — agent infrastructure; `skynet/MCP-shared-graph-memory/` is the
  shared memory module (Store, scan/coverage tools, [SKILL.md](skynet/MCP-shared-graph-memory/SKILL.md),
  [docs/CODEGRAPH.md](skynet/MCP-shared-graph-memory/docs/CODEGRAPH.md));
  [skynet/knowledge-map.md](skynet/knowledge-map.md) is the knowledge index.
- `skills/{profiles,shared,superpowers}/` — canonical agent skills;
  `.opencode/skills/` holds active copies (sync rules in its README).

## 2. File naming

- Use `-` (not `_`) in C/C++ source file names and directory names; use
  `.c++` / `.h++` extensions for C++ sources.
- Use `_` (not `-`) in Python script names.
- Do not keep stale or duplicating documentation files; outdated content is
  merged into the canonical file and the duplicate is removed.
- Test-file naming, structure and the test-suite guidelines live in
  [tests/AGENTS.md](tests/AGENTS.md).

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

- Follow the [superpowers](skills/superpowers/README.md) methodology flow:
  brainstorm → plan → worktree → TDD → review → finish.
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
- Do not take an issue that is assigned to another developer/user without
  an explicit instruction (from the owner or that assignee); in a
  multi-agent collaboration pick only unassigned issues or ones explicitly
  handed to you.
- Test first: if the issue already has a reproduction scenario, add a test
  modeled on `tests/ut/issues/**` — it must fail.
- Then implement the fix; the test must pass afterwards.
- The test-suite specifics (naming, hygiene/cleanup rules) live in
  [tests/AGENTS.md](tests/AGENTS.md).
- The fix includes an adequate ChangeLog entry in a user-facing form —
  see `docs/engineering/changelog-codex.md` and section 6 below.
- External reporters are thanked in the ChangeLog `Appreciations` section
  (follow existing entries via `grep Appreciations ChangeLog.md`); never
  mention a user more than once — merge mentions logically.
- External communications rules: [docs/engineering/issues-pr-codex.md](docs/engineering/issues-pr-codex.md).

## 6. ChangeLog rules

Canonical style guide: [docs/engineering/changelog-codex.md](docs/engineering/changelog-codex.md).
The format follows [Keep a Changelog 2.0.0](https://keepachangelog.com/en/2.0.0/)
with the house deviations listed here.

Scope and curation:

- Only user-affecting changes (reaching the amalgamated build) go into
  `ChangeLog.md`; anything else only in exceptional cases, agreed with the
  owner. Curate, don't accumulate: the ChangeLog is a summary of notable
  changes for readers, not a record of commits.
- One cause — one entry: related fixes from one issue or one rework are a
  single entry listing the affected APIs, not a line per function.

Structure:

- A version section uses only the Keep-a-Changelog types: `Added`,
  `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`, plus the house
  `Appreciations` section. No other top-level sections; an empty type is
  omitted.
- Every entry begins with the verb of its type: `Added …`, `Changed …`,
  `Deprecated …`, `Removed …`, `Fixed …`; a `Security` entry begins with
  its CVE identifier when present.
- `Fixed` keeps thematic subsections (a house deviation users asked for);
  subsections exist only inside `Fixed`, follow the established taxonomy
  and stay few and stable.
- A version opens with a summary of one or two plain sentences: what the
  release gives the user.
- Breaking changes are marked `**Breaking:**` on the entry itself, within
  its type — never collected into a separate section.
- The version under development is the upcoming version's section with the
  `at a deep development stage` date placeholder; a released version is
  `## vX.Y.Z at YYYY-MM-DD`.

Wording:

- User-facing wording: no internal jargon, no low-level details — describe
  what the user would have observed; a brief mention of the fixed problem
  as the user could see it is welcome, "bird language" and detailed
  explanations of minutiae are not.
- An entry is a single sentence of up to 100 characters, not counting the
  trailing issue links. Longer entries (2–3 sentences or a short
  structural note) are allowed only for large reworks and only with the
  owner's direction or confirmation. Every entry must be semantically
  complete and end with a period.
- Backports get ` (backport)` immediately before the final period.
- Issues must be referenced only as markdown links (GitHub, SourceCraft or
  third-party); bare `#N` mentions are noise and must not appear anywhere,
  including commits and code.
- Internal identifiers, field names and abbreviations are not acceptable
  (rare exceptions allowed).

House deviations from Keep a Changelog: the `Appreciations` section;
thematic subsections inside `Fixed`; issue links inline at the end of
entries; `at a deep development stage` instead of `[Unreleased]`. Place a
new entry in the appropriate existing type (or an existing `Fixed`
subsection); when nothing fits, propose a new `Fixed` subsection and ask
when in doubt.

## 7. Code style

- LLVM code style, **2-space indent**, `ColumnLimit: 120` (see
  [.clang-format](.clang-format) in the repository root; run it before
  publishing).
- Minimize preprocessor conditionals: do not introduce new
  `#if`/`#else`/`#endif`; migrate existing ones into system-dependent
  modules (`src/osal/osal.h`, platform files) gradually. Prefer fallback
  `#define`s resolving to no-op functions/macros and runtime
  `MDBX_OPTION_*`-style branches over compile-time cuts (e.g. the
  `MPROBE_*` probes are no-op outside `MDBX_PROBES` builds without any
  `#if` at the call site).
- Probes/instrumentation are welcome, but never gated by `#if`.
- Cross-platform correctness, including tests: never compare page-size or
  derived values against constants — it is platform-dependent.

## 8. Build & CI hygiene

- Build and test on **at least Linux and Windows** — build support for both
  platforms is expected for any change (see also
  [CONTRIBUTING.md](CONTRIBUTING.md)).
- Build with `ninja` and `ccache`; run tests in parallel (`-j 4`).
- Do not trigger unnecessary CI iterations; do not wait for red/failing
  runs — go fix them; cancel obsolete CI runs after merges.
- Promptly remove temporary directories and other debris; monitor free
  disk space before and after test runs. Tests must clean up their own
  database files — see [tests/AGENTS.md](tests/AGENTS.md) §3.

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
- Testing documentation (rules, methodology, scenario specs, coverage,
  durability modes, CI v2 infrastructure design) lives in
  [tests/AGENTS.md](tests/AGENTS.md) §5.
- [docs/engineering/probe-bus.md](docs/engineering/probe-bus.md) — probe
  conventions.
- [docs/engineering/libmdbx-invariants.md](docs/engineering/libmdbx-invariants.md),
  [docs/engineering/module-lock-matrix.md](docs/engineering/module-lock-matrix.md) —
  mandatory context for review profiles.
- [skynet/MCP-shared-graph-memory/SKILL.md](skynet/MCP-shared-graph-memory/SKILL.md),
  [skynet/MCP-shared-graph-memory/docs/CODEGRAPH.md](skynet/MCP-shared-graph-memory/docs/CODEGRAPH.md),
  [skynet/MCP-shared-graph-memory/tools/scan_all.py](skynet/MCP-shared-graph-memory/tools/scan_all.py) —
  structural map, multi-platform scanning and coverage pipeline.
- [skynet/knowledge-map.md](skynet/knowledge-map.md) — knowledge index and
  memory procedures.

## 10. Agent skills profiles

Skills live in [`skills/`](skills/README.md) with active copies in
`.opencode/skills/` (usable through the `skill` tool). Pick a profile by
task and load it via the tool:

| Task | Profile |
|---|---|
| New API function / WAF optimization | `developer` |
| Deep refactoring (B+tree/CoW/GC/API) | `refactoring` |
| Test decomposition, characterisation, CI pipeline | `testing` |
| Code review (C/CXX/ARCH) | `code-review` |
| Build, CI, amalgamation, NDK, LTO | `platform-build` |
| Documentation / ADR maintenance | `documentation` |

Every profile's mandatory context references `docs/engineering/*`
(`libmdbx-invariants.md`, `subsystem-map.md`, `module-lock-matrix.md`,
`test-durability-strategy.md`); read them by link, do not duplicate
content.

## 11. Skills update & session restart

Skills are loaded into opencode sessions at startup. When you change
`skills/*` or `.opencode/skills/*`:

- sync the active copies in `.opencode/skills/` (relative-link depth
  differs there);
- mark the commit/PR with `SKILLS-UPDATED` and note "restart sessions
  required";
- refresh the `skill-tree` worktree (`git fetch && git reset --hard
  origin/<branch>`);
- the coordinator restarts canonical sessions afterwards.

Full workflow: [skynet/skill-tree.md](skynet/skill-tree.md).
