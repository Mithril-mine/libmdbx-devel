# AGENTS.md — Agent Guidelines

> **Dev-only** guidance for AI agents and developers; part of the development
> tree, **not** included in `make dist` artifacts. `GIGACODE.md` is a symlink
> to this file: it is the single instruction file for every agent harness.

This is the **libmdbx-devel** development tree of _libmdbx_, an embedded
transactional key-value database. The `make dist` artifacts in the root
(`dist/`, `@dist-check/`, `@dist-check.log`, `libmdbx-sources-*.tar.gz`) are
generated and git-ignored — never edit or commit them. See also
[CONTRIBUTING.md](CONTRIBUTING.md), the
[documentation index](docs/engineering/README.md) and the
[project knowledge base](skynet/knowledge-map.md).

## Direction: test coverage → refactoring → C++ → MithrilDB

Correctness and performance are what the project lives on; its performance
rests on deliberate micro-optimizations (§8). When rules or habits conflict:
correctness and reliability > performance > clarity and simplicity, while
concision and reuse hold always (§8). The order of work is fixed:

1. **Test coverage and test infrastructure — the current stage.** The
   refactoring of the engine (C11) as a whole waits until coverage is high
   enough. The suite is moving to C++ (GoogleTest) and onto the testing API
   (`tests/api.{h,c}`).
2. **Refactoring** of the engine, under characterisation tests.
3. **C++**: the core is ported while the C API ABI holds — the reverse of
   today's dependency, where `mdbx.h++` wraps the C core
   ([libmdbx-invariants.md](docs/engineering/libmdbx-invariants.md) §4).
4. **MithrilDB**: the next generation, a new format and a C++20 API
   ([README.md](README.md), "MithrilDB and Future").

Hence, for every change:

- **Tests lead**: a change to the library ships with a C++ test pinning the
  behaviour it touches, written against the public API or the testing API.
- **Engine-wide refactoring, porting and architecture work are not
  self-started**: until that stage opens, an engine change is a fix or a
  targeted optimization that adds the least new code, reusing and extracting
  what already exists (§8.5).
- **Decisions are the owner's**: what gets built — features, options, API,
  ABI, on-disk format, architecture, a new stage — is proposed by an agent,
  never decided.

## 1. Repository layout pointers

The structure descriptions are maintained elsewhere — do not duplicate them
here:

- `src/` — the library sources; `mdbx++/` — the C++ API layer (`decl-*.h++` /
  `impl-*.h++`); [docs/engineering/structure.md](docs/engineering/structure.md),
  [docs/engineering/cxx-api.md](docs/engineering/cxx-api.md).
- `tests/` — the test suite; its guidelines (layout, naming, hygiene/cleanup)
  live in [tests/AGENTS.md](tests/AGENTS.md).
- `docs/engineering/` — engineering documentation: the canonical codexes
  (workflows, changelog, issues/PR, testing, coverage).
- `skynet/` — agent infrastructure; `skynet/MCP-shared-graph-memory/` is the
  shared memory module (Store, scan/coverage tools,
  [SKILL.md](skynet/MCP-shared-graph-memory/SKILL.md),
  [CODEGRAPH.md](skynet/MCP-shared-graph-memory/docs/CODEGRAPH.md));
  [skynet/knowledge-map.md](skynet/knowledge-map.md) is the knowledge index.
- `skills/{profiles,shared,superpowers}/` — canonical agent skills;
  `.opencode/skills/` holds active copies (sync rules in its README).
- Before changing the engine, read
  [libmdbx-invariants.md](docs/engineering/libmdbx-invariants.md),
  [subsystem-map.md](docs/engineering/subsystem-map.md),
  [module-interfaces.md](docs/engineering/module-interfaces.md) and
  [module-lock-matrix.md](docs/engineering/module-lock-matrix.md) — the
  mandatory context of the review profiles.

## 2. File naming

- `-` (not `_`) in C/C++ source file and directory names; `.c++` / `.h++`
  extensions for C++ sources.
- `_` (not `-`) in Python script names.
- No stale or duplicating documentation files: outdated content is merged into
  the canonical file and the duplicate removed.
- Test-file naming, structure and the test-suite guidelines live in
  [tests/AGENTS.md](tests/AGENTS.md).

## 3. Git history

Canonical policy: [skynet/git-history-policy.md](skynet/git-history-policy.md).

- Every commit is working and CI-green, so `git bisect` never follows false
  paths: no temporary commits, drafts or work-in-progress snapshots — only
  final, working code.
- A fix is folded into its originating commit (amend, fixup + autosquash),
  never stacked on top as a separate "fix" commit. Test additions only extend
  existing tests; corrections are squashed.
- Never rewrite across git tags; ask the owner before rewriting deeper than a
  few recent commits.

## 4. General workflow

- Method: [superpowers](skills/superpowers/README.md) — brainstorm → plan →
  worktree → TDD → review → finish.
- Work by git-flow: branch → PR → review/CI. Before merging: the history is
  clean (fix commits folded into their sources) and **all** reviewer and bot
  comments are resolved.
- Merging: prefer rebase/fast-forward; a merge commit is acceptable only when
  the PR carries several independent, non-trivial changes.
- A PR references every related item/issue and carries a GitHub-recognized
  closing signature (e.g. `Closes #N`) when it fully fixes one.
- Comments posted to issues/PRs start with the line `// skynet here`. Comment
  on related issues, close fixed ones, answer comments, and thank contributors
  for clarifications.

## 5. Work organization — worktrees, session scope, PR lifecycle (MANDATORY)

- Work only inside your own worktree/workset; never modify anything outside
  it — other agents' worktrees, foreign session directories, or the main
  checkout's working files.
- A worktree/workset you create lives **inside the session's main directory** —
  not alongside it or anywhere else.
- When the work is finished, clean up everything: remove the temporary
  worktree, delete its temporary branches, and remove every temporary file it
  produced.
- Every pull request is opened as a **draft** and moves to
  **ready-for-review** only after the work is complete and its CI is green.
- A `ready-for-review` PR that violates any rule of this file may be deleted
  by the bot together with its branch.

## 6. Issues workflow

- When taking an issue, assign it to yourself.
- Do not take an issue assigned to another developer/user without an explicit
  instruction (from the owner or that assignee); in a multi-agent
  collaboration pick only unassigned issues or ones explicitly handed to you.
- Test first: if the issue already has a reproduction scenario, add a test
  modeled on `tests/ut/issues/**` — it must fail. Then implement the fix; the
  test must pass afterwards.
- The test-suite specifics (naming, hygiene/cleanup rules) live in
  [tests/AGENTS.md](tests/AGENTS.md).
- The fix includes an adequate ChangeLog entry in a user-facing form — see
  [docs/engineering/changelog-codex.md](docs/engineering/changelog-codex.md)
  and section 7 below.
- External reporters are thanked in the ChangeLog `Appreciations` section
  (follow the existing entries via `grep Appreciations ChangeLog.md`); never
  mention a user more than once — merge mentions logically.
- External communications rules:
  [docs/engineering/issues-pr-codex.md](docs/engineering/issues-pr-codex.md).

## 7. ChangeLog rules

Canonical style guide:
[docs/engineering/changelog-codex.md](docs/engineering/changelog-codex.md).
The format follows [Keep a Changelog 2.0.0](https://keepachangelog.com/en/2.0.0/)
with the house deviations listed here.

- **Scope**: only user-affecting changes (reaching the amalgamated build) go
  into `ChangeLog.md`; anything else only in exceptional cases, agreed with
  the owner.
- **Curation**: curate, don't accumulate — the ChangeLog is a summary of
  notable changes for readers, not a record of commits.
- **One cause — one entry**: related fixes from one issue or one rework are a
  single entry listing the affected APIs, not a line per function.
- **Types**: a version section uses only the Keep-a-Changelog types `Added`,
  `Changed`, `Deprecated`, `Removed`, `Fixed`, `Security`, plus the house
  `Appreciations` section. No other top-level sections; an empty type is
  omitted.
- **Verbs**: every entry begins with the verb of its type (`Added …`,
  `Changed …`, `Deprecated …`, `Removed …`, `Fixed …`); a `Security` entry
  begins with its CVE identifier when present.
- **`Fixed` subsections**: `Fixed` keeps thematic subsections (a house
  deviation users asked for); subsections exist only inside `Fixed`, follow
  the established taxonomy and stay few and stable.
- **Version summary**: a version opens with a summary of one or two plain
  sentences — what the release gives the user.
- **Breaking changes** are marked `**Breaking:**` on the entry itself, within
  its type — never collected into a separate section.
- **Version headings**: the version under development is the upcoming
  version's section with the `at a deep development stage` date placeholder; a
  released version is `## vX.Y.Z at YYYY-MM-DD`.
- **Wording**: user-facing — no internal jargon, no low-level details; describe
  what the user would have observed. A brief mention of the fixed problem as
  the user could see it is welcome; "bird language" and detailed explanations
  of minutiae are not.
- **Length**: a single sentence of up to 100 characters, not counting the
  trailing issue links, semantically complete and ending with a period. Longer
  entries (2–3 sentences or a short structural note) only for large reworks,
  and only with the owner's direction or confirmation.
- **Backports** get ` (backport)` immediately before the final period.
- **Issues** are referenced only as markdown links (GitHub, SourceCraft or
  third-party), inline at the end of the entry; bare `#N` mentions are noise
  and must not appear anywhere, commits and code included.
- **Identifiers**: internal identifiers, field names and abbreviations are not
  acceptable (rare exceptions allowed).
- **House deviations** from Keep a Changelog: the `Appreciations` section;
  thematic subsections inside `Fixed`; issue links inline at the end of
  entries; `at a deep development stage` instead of `[Unreleased]`.
- **Placement**: a new entry goes into the appropriate existing type (or an
  existing `Fixed` subsection); when nothing fits, propose a new `Fixed`
  subsection and ask when in doubt.

## 8. Code style, correctness and performance

Four rules hold for all code at all times:

- **Minimize the number of operations and conditional branches** — always,
  most of all in the hot paths and in the functions marked `__hot` (§8.4).
- **Strive for concise and reusable code** — always, refactoring to extract
  common fragments into functions (§8.5).
- **Strive for clear and simple code** — as long as it costs nothing in
  performance and reliability (§8.5).
- **Apply the obvious optimization tactics and the known efficient
  algorithms** — always, replacing inefficient code (§8.4).

The hot paths are hand-tuned (`__hot`/`__cold`, thousands of
`likely()`/`unlikely()`): performance here rests on deliberate
micro-optimizations.

### 8.1 Formatting and warnings

- **Style**: LLVM code style, **2-space indent**, `ColumnLimit: 120` (see
  [.clang-format](.clang-format) in the repository root); run
  `tests/ci/check-clang-format.sh` before publishing.
- **Warnings**: the build stays clean under `-Wall -Wextra -Werror` (CI enables
  it) — a new warning is a defect, not noise. Do not paper over a warning with
  a cast, a pragma or a widened type when the construct itself is wrong; a
  suppression needs a comment proving that the warning is a false positive at
  that exact place.

### 8.2 Preprocessor

- **Conditionals**: minimize preprocessor conditionals — do not introduce new
  `#if`/`#else`/`#endif`; migrate existing ones into system-dependent modules
  (`src/osal/osal.h`, platform files) gradually. Prefer fallback `#define`s
  resolving to no-op functions/macros and runtime `MDBX_OPTION_*`-style
  branches over compile-time cuts (e.g. the `MPROBE_*` probes are no-op outside
  `MDBX_PROBES` builds without any `#if` at the call site).
- **Probes**: probes and instrumentation are welcome, but never gated by `#if`.
- **Page size**: cross-platform correctness, including tests — never compare
  page-size or derived values against constants; it is platform-dependent.
- **Options**: a compile-time option changes cost, never semantics: a build
  with the option off still builds and passes the scope that makes sense for
  it; where a test only exists with the option on, the registration is gated
  in CMake and the missing coverage is stated
  ([tests/AGENTS.md](tests/AGENTS.md), "Coverage honesty").
- **Word width**: a word-width or capability difference is a runtime check
  (`sizeof`, `MDBX_WORDBITS`, `MDBX_OPTION_*`), not a compile-time cut: the
  32-bit legs must stay reachable from the same source — the win32/armeabi CI
  cells rely on it.

### 8.3 Correctness: units, word width, alignment

The defects that reach users are almost never typos — they are unit, width and
aliasing mistakes that compile quietly on the other platform.

- **Cross-platform correctness**: never compare a page size or a derived value
  against a constant — it is platform-dependent; take the actual value from
  the environment or the library, and state the unit.
- **A unit belongs to the name and to the type contract.** A byte count, a page
  count and an entry count are different things; never pass one where another
  is expected. Before converting, read what the helper already returns:
  `MDBX_PNL_SIZEOF()` and `gc_chunk_bytes()` already yield bytes, `pnl_size()`
  yields entries, `pgno2bytes()` yields bytes — a "fix" that converts a second
  time multiplies a page reservation instead of repairing it.
- **Word width is explicit.** `size_t` is the host address width, `uint64_t`
  the on-disk value width; products of on-disk quantities (page count ×
  pagesize, offsets, txnids) are formed in 64 bits and compared with the host
  envelope (`INTPTR_MAX`, `globals.mmap_limit`) before narrowing. Such a
  product wraps *silently*: every calculation of this kind is re-checked on a
  32-bit target.
- **One source of truth per computation.** Two paths forming the same value
  share a helper (the two geometry-from-db branches of `dxb_setup()` diverged,
  only one was hardened, the other kept wrapping). Where a helper's arithmetic
  is unusable (a `size_t` helper where 64 bits are needed), put the twin next
  to the original, name both in the comments and share the limits — never copy
  the block.
- **Alignment and aliasing.** A `char`/`uint8_t` buffer cast to `page_t *`,
  `meta_t *` or a `uint64_t` field requires `MDBX_ALIGNAS(8)`: a plain array
  may legally sit at any byte address, which trips the `unaligned_peek()`
  assertion and leaves a misaligned access in a release build.
  `unaligned_peek/poke(N, ...)` names the alignment it relies on; the caller
  guarantees it.
- **Atomics**: through `src/atomics-ops.h` and the `osal_fence_*` ladder,
  never hand-rolled — `osal_fence_consumer()` goes after the ordered read,
  `osal_fence_producer()` before the ordered write.
- **Failure is reported, not hidden.** Every fallible call is checked and the
  engine's own code is propagated (`LOG_IFERR`); a generic `-1`, a swallowed
  error or a silent fallback that conceals a refusal is a defect in disguise —
  the user must see `MDBX_INCOMPATIBLE`, not a database opened with a zeroed
  field.
- **Both legs of a conditional structure are reviewed.** RO/RW,
  exclusive/shared, 32/64-bit, recovery/fast-path: a hardening applied to one
  branch is checked against the other, or the difference is stated in the
  comment.
- **API, ABI and the on-disk format are contracts.** A public signature, a
  struct layout, an on-disk field or a lock-file format is not changed without
  the owner's decision; the invariants and the ABI boundary live in
  [docs/engineering/libmdbx-invariants.md](docs/engineering/libmdbx-invariants.md)
  §4, the module map in
  [docs/engineering/module-interfaces.md](docs/engineering/module-interfaces.md).
  A format change comes with the crafted-format corpus update, the docs and a
  ChangeLog note in the same PR; a lock-file format change additionally states
  the upgrade path for an already-existing environment.
- **Module interfaces**: a function used across modules is declared in
  `src/proto.h` or its module header — in the alloy build an undeclared one
  turns `static` and is lost. A function moved between modules is checked in
  both build modes, alloy and `-DMDBX_ALLOY_BUILD=OFF`. The C core never
  depends on `mdbx.h++` or `mdbx++/`.

### 8.4 Performance

- **Hot path discipline**: keep the hot paths free of allocation, I/O, logging
  and validation — b-tree search/insert/delete, cursor movement, commit and
  page allocation run without a lazy allocation, a `fprintf` or a debug-only
  branch. The cold/diagnostic parts live in `__cold` helpers; the hot helpers
  are marked `__hot` (or inlined) to guide the compiler's code placement. The
  compile-time knobs (`MDBX_DEBUG`, `MDBX_CHECKING`, `MDBX_ENABLE_PGOP_STAT`)
  change the cost, never the semantics.
- **Indirect calls** cost a branch-prediction slot: the built-in fast path
  (comparator, key transform, page access) stays direct, and a custom callback
  is dispatched outside the inner loop or behind a cold/opt-in branch.
- **Code size**: prefer smaller code on a hot path over an unrolled or
  duplicated variant unless a measurement says otherwise — instruction-cache
  pressure is a real cost in the b-tree loops.
- **Operations and branches**: always minimized, most of all in the hot paths
  and in the functions marked `__hot` — order conditions by probability, merge
  consecutive guards into one predicate, use arithmetic or a table where the
  "switch" is a value classification, and hand the rare case to a
  `__cold`/`__hot` branch. `likely()`/`unlikely()` mark a genuinely skewed
  branch — as decoration they cost as much as the branch they were meant to
  remove.
- **Optimization tactics**: the obvious ones and the known efficient
  algorithms are applied always, and inefficient code is replaced. An
  optimization is reasoned out from what the code does; it does not wait for a
  profiler run.
- **Rare cases**: do not add a branch, a call, a lock or a memory barrier to a
  hot path to serve a rare case; serve that case from a separate (cold) path.
- **Memory layout**: keep it tight — cacheline alignment for contended atomics
  and locks, exact packing where the on-disk format requires it, no accidental
  padding in per-reader/per-page structures; the `sizeof` of hot structures is
  pinned by `STATIC_ASSERT`s where the format depends on it.
- **Deliberate code**: an odd construct on a hot path — branchless comparison,
  unrolling, `__restrict`, packed layout — is a micro-optimization. Find its
  origin (`git log -S`, `git blame`, the linked issue) before simplifying it.
- **Cost**: a hot-path change states in its commit what it adds or removes —
  branches, indirect calls, memory accesses, struct bytes. Timing measurements
  are not required for a routine change: `make bench` takes 30–120 minutes and
  host noise can exceed the effect many times over.

> *Note*: write amplification (WAF) per single-point update is an architectural
> property of the MVCC+CoW B+tree (≈ tree height) and is not a per-commit
> metric — it is governed by higher-level algorithms, not by local code changes.

### 8.5 Compactness and clarity

- **Concision and reuse**: always strive for concise, reusable code — refactor
  to extract common fragments into functions.
- **Clarity and simplicity**: always strive for clear and simple code, as long
  as it costs nothing in performance and reliability.
- **No hypothetical generality**: no unused parameter, option, "reserved" field
  or abstraction with a single call site. Dead, disabled and commented-out code
  is deleted — git remembers it.
- **No duplicated helper or constant**: reuse `src/utils.h`, `src/cogs.h`,
  `src/unaligned.h`; a second copy of `ceil_powerof2()`-style semantics is
  exactly how two branches drift apart (see §8.3).
- **Functions** do one thing and fit on a screen: early returns over nested
  `if`s, `const` on read-only parameters and locals, and a name that carries
  the unit and the ownership (`allocated_bytes`, `retired_pgno`).
- **Comments** explain *why* and must be true: a wrong rationale ("a wider
  parameter zero-extends the value and changes the count" — it does not) is
  worse than no comment. Cite the mechanism, the invariant or the issue; never
  restate the code.
- **Constants**: a magic number gets a named constant and a derivation;
  on-disk offsets come from `offsetof()` and are pinned with `STATIC_ASSERT`,
  never hand-computed.
- **Stale references** are bugs: after moving or renaming anything, `git grep`
  the old path and update the comments in `src/**`, `tests/api.*`,
  `docs/engineering/*` and the lint pin lists in the same commit.

### 8.6 Change discipline

- **One cause — one commit and one PR**: an unrelated CI tweak, a formatting
  sweep or a timeout bump does not ride inside a fix (it hides the fix's own
  risk and makes `git bisect` less useful); a defect found on the way gets its
  own issue.
- **Backports** are re-derived for this branch, not translated: check the
  branch's own helpers and units — a faithful-looking port can be a no-op, or
  can apply a conversion twice. When the upstream fix has no counterpart here,
  drop it and say so in the PR.
- **Test hooks**: library test hooks go into the testing API (`tests/api.{h,c}`,
  see [tests/AGENTS.md](tests/AGENTS.md) §8), never into the public headers
  and never behind an `#if` in the sources.
- **PR body**: states what was verified and how — build types, `ctest`
  scopes/labels, CI cells, measurements — not only what changed.

## 9. Build & CI hygiene

- Build and test on **at least Linux and Windows** — build support for both
  platforms is expected for any change (see also
  [CONTRIBUTING.md](CONTRIBUTING.md)).
- Build with `ninja` and `ccache`; run tests in parallel (`-j 4`).
- Do not trigger unnecessary CI iterations; do not wait for red/failing runs —
  go fix them; cancel obsolete CI runs after merges.
- Promptly remove temporary directories and other debris; monitor free disk
  space before and after test runs. Tests must clean up their own database
  files — see [tests/AGENTS.md](tests/AGENTS.md) §4.
- Keep the test build tree on a disk-backed filesystem: on tmpfs the library
  logs the `in-core database` NOTICE, which only marked golden lines tolerate.
- `make dist` stays green: dev-only material lives inside `dist-cutoff`
  markers, and a file that must ship is listed in `DIST_SRC`/`DIST_EXTRA`
  ([docs/engineering/build.md](docs/engineering/build.md) §7).
- After a rename, a move or a registration change, `git grep` the old name
  before pushing: a stale reference in `src/*.h` comments, in
  `docs/engineering/*` or in a lint pin list is a defect (§8.5).
- A red cell is diagnosed from its log, not re-run; the PR body carries the
  verification evidence (§8.6), and a "green but skipped" unit is not evidence
  at all. A check is reported as passed only if it ran — a platform or level
  not run is named as such.

### 9.1 Testing instructions

Rules: [tests/AGENTS.md](tests/AGENTS.md). Options, targets, amalgamation and
gotchas: [docs/engineering/build.md](docs/engineering/build.md).

**Verification levels** — escalate as far as the change warrants
([docs/engineering/workflows.md](docs/engineering/workflows.md) §3): L1 the
specific test → L1-x the same test under a sanitizer → L2 all `ctest` → L3
`make check` (smoke, install, amalgamation) → L4 `make test-asan` /
`make test-ubsan` → L5 `make test-memcheck` → L6 `tests/stochastic.sh` → L7 a
human-supervised soak. After touching `src/` reach at least L4; after changing
Doxygen comments run `make test-doxygen`; for a locking change add
`make check-posix-locking`. `@cmake-ninja-build` is the tree that `make ninja`,
`make ctest` and the smoke scenarios reuse.

```sh
cmake -G Ninja -S . -B @cmake-ninja-build -DMDBX_ENABLE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug \
  -DINTERPROCEDURAL_OPTIMIZATION=OFF -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build @cmake-ninja-build
B=@cmake-ninja-build
```

**Segmented test execution** — select a scope (`ctest -N -L '<label>'` lists
it) instead of running the whole corpus for a routine change:

```sh
ctest --test-dir $B -j 4 --output-on-failure -L 'ut\.' -LE 'ut\.heavy|chk'   # the fast unit scope
ctest --test-dir $B -j 4 -L 'ut\.cxx'                                         # one area: api cxx dbi txn cursor gc env details issues tools durability
ctest --test-dir $B -R '^txn-dataops$'                                        # one unit; an issue regression is issue_ghNNNN, a tool's golden run ut_tools_<tool>
tests/select-tests.sh --base <remote>/<branch>                                # changed paths -> the ctest -L line (printed; the default base origin/devel is often absent)
ctest --test-dir $B -L hygiene                                                # leftovers scan and testing-API lint: after the corpus, never alongside it
```

```sh
tests/ci/check-clang-format.sh                       # tracked C/C++ vs .clang-format (`make reformat` applies it)
tests/ci/check-testing-api.py                        # testing-API pins and the sanctioned src/ inclusions
python3 tests/ci/validate-config.py check-structure  # CI cell registry; then with check-coverage
make test-doxygen                                    # strict audit of the public-header Doxygen comments (doxygen >= 1.18)
make check                                           # clean, smoke+assertions, dist, install, test, ctest, doxygen
make test-asan && make test-ubsan                    # sanitizers; make test-memcheck for Valgrind
```

Test rules that bind every change (the rest is in
[tests/AGENTS.md](tests/AGENTS.md)):

- A test exercises the real library: a copy of library code, or a direct
  `#include` from `src/` outside `tests/api.c`, is not a test.
- Every test removes its database files on every exit path (`env_files_guard`)
  and carries an explicit CTest timeout (`tests/test-timeouts.cmake`).
- A test fails before the fix and passes after, on both host widths; it is
  order-independent and listed in its CI scope.

## 10. Documentation map

- [docs/engineering/README.md](docs/engineering/README.md) — documentation
  index.
- [docs/engineering/workflows.md](docs/engineering/workflows.md) — branching,
  PR/review flow, local validation, CI triggers.
- [skynet/git-history-policy.md](skynet/git-history-policy.md) — history
  rewrite policy.
- [docs/engineering/changelog-codex.md](docs/engineering/changelog-codex.md),
  [docs/engineering/issues-pr-codex.md](docs/engineering/issues-pr-codex.md) —
  comms and ChangeLog codexes.
- [docs/engineering/build.md](docs/engineering/build.md) — build options and
  targets, testing in depth, amalgamation (`make dist`), gotchas.
- Testing documentation (rules, timeouts, hygiene, methodology, scenario
  specs, coverage, durability modes, CI infrastructure design) lives in
  [tests/AGENTS.md](tests/AGENTS.md), "Documentation map (testing)".
- [docs/engineering/probe-bus.md](docs/engineering/probe-bus.md) — probe
  conventions.
- [docs/engineering/libmdbx-invariants.md](docs/engineering/libmdbx-invariants.md),
  [docs/engineering/module-lock-matrix.md](docs/engineering/module-lock-matrix.md) —
  mandatory context for review profiles.
- [skynet/MCP-shared-graph-memory/SKILL.md](skynet/MCP-shared-graph-memory/SKILL.md),
  [docs/CODEGRAPH.md](skynet/MCP-shared-graph-memory/docs/CODEGRAPH.md),
  [tools/scan_all.py](skynet/MCP-shared-graph-memory/tools/scan_all.py) —
  structural map, multi-platform scanning and coverage pipeline.
- [skynet/knowledge-map.md](skynet/knowledge-map.md) — knowledge index and
  memory procedures.

## 10.5. MCP servers

Two MCP servers are installed locally and registered in the developer's MCP
config (e.g. `~/.config/opencode/opencode.json`); opencode loads both
automatically at session startup, and the skills in `skills/` and
`.opencode/skills/` complement them. Configuration differs per developer and
per harness — the roles, not installation paths:

### `codebase-memory`

- **Purpose**: structural code queries via the project's knowledge graph —
  tracing call chains, dependency analysis, structural maps, dead-code
  detection, impact analysis.
- **Skills**: the largest skill set bundled with this server
  (`codebase-memory`, `code-reviewer`, `debugging-assistant`,
  `discovery-process`, `doc-coauthoring`, `idea-refine`, `jobs-to-be-done`,
  `notes-synthesis`, `prioritization-advisor`, `product-brainstorming`,
  `prompt-improver`, `repository-analyzer`, and others).
- **Config section**: `"codebase-memory"` in `opencode.json`.

### `shared-graph-memory.mdbx`

- **Database**: MDBX-based shared memory for the project's knowledge graph
  (path set via the `SHARED_GRAPH_MEMORY_PATH` environment variable).
- **Purpose**: project-wide knowledge store — persistent facts, decisions,
  procedures, and archives managed across agent sessions; includes backup
  tiers, scan/coverage tools, and `SKILL.md` for memory procedures.
- **Repository source**: `skynet/MCP-shared-graph-memory/` — Store,
  scan/coverage tools, [SKILL.md](skynet/MCP-shared-graph-memory/SKILL.md),
  [CODEGRAPH.md](skynet/MCP-shared-graph-memory/docs/CODEGRAPH.md),
  [scan_all.py](skynet/MCP-shared-graph-memory/tools/scan_all.py).
- **Knowledge index**: [skynet/knowledge-map.md](skynet/knowledge-map.md),
  `skynet/knowledge-records.yaml`.
- **Config section**: `"shared-graph-memory.mdbx"` in `opencode.json`.

### Skills mapping

Skills in `skills/{profiles,shared,superpowers}/` are canonical copies;
`.opencode/skills/` holds active copies with sync rules (see its README). Each
server's skills are loaded independently through the `skill` tool.

## 11. Agent skills profiles

Skills live in [`skills/`](skills/README.md) with active copies in
`.opencode/skills/` (usable through the `skill` tool — or read
`skills/profiles/<name>/SKILL.md` where the harness registers none). Pick a
profile by task and load it:

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
`test-durability-strategy.md`); read them by link, do not duplicate the
content.

## 12. Skills update & session restart

Skills are loaded into opencode sessions at startup. When you change
`skills/*` or `.opencode/skills/*`:

- sync the active copies in `.opencode/skills/` (relative-link depth differs
  there);
- mark the commit/PR with `SKILLS-UPDATED` and note "restart sessions
  required";
- refresh the `skill-tree` worktree (`git fetch && git reset --hard
  origin/<branch>`);
- the coordinator restarts canonical sessions afterwards.

Full workflow: [skynet/skill-tree.md](skynet/skill-tree.md).

## 13. Campaign findings: expressive slugs, no session-scoped identifiers in tracked files

Findings, lemmas, work-items and tests introduced by a verification/audit
campaign or an agent session (`V-042`, `F-V9-3`, `wave-5`, `SC-97`, `RF-4`,
`B54`) are session-scoped: meaningful only within that session's materials,
they turn the documentation into a puzzle once several campaigns accumulate.

- Repository-tracked files MUST NOT use such identifiers. Name a finding by an
  expressive slug (e.g. `windows-readonly-mmap-extent-fixed-at-open`,
  `load-nul-byte-silent-truncation`) and give it a self-contained description
  at the place of use — the reader must understand the item without the
  campaign materials or GitHub issues.
- The session-scoped identifiers may live in the campaign's working artifacts
  (temporary branches/ledgers) and in GitHub issues/discussions. An issue that
  survives the campaign is self-contained in the same way: full descriptions,
  slugs for handles, and at most a historical cross-reference to the
  campaign's materials.
- The shared-graph-memory store summaries are exported into git
  (`skynet/knowledge-map.md`, `skynet/knowledge-records.yaml`) and therefore
  follow the same rule — write store summaries in the slug language, not in
  campaign-internal ids.
- A permanent rule of the engineering docs: a proven invariant or a contract
  enters `libmdbx-invariants.md` / `module-interfaces.md` under its own
  descriptive lemma name, never under a campaign id.

## 14. Pre-publish self-review checklist

Run this before requesting review; the profiles
[`requesting-code-review`](skills/superpowers/requesting-code-review/SKILL.md)
and [`verification-before-completion`](skills/superpowers/verification-before-completion/SKILL.md)
carry the long form.

**Correctness and quality (§8):**

- [ ] the four standing rules of §8 hold: operations and conditional branches
  minimized, common fragments extracted into functions, the obvious
  optimization tactics applied, clarity bought at no cost in speed;
- [ ] units, word widths and alignments re-checked (§8.3) — including a 32-bit
  target for anything that multiplies on-disk quantities;
- [ ] each computation exists once; no helper was copied, no duplicated constant
  was introduced;
- [ ] both legs of every conditional structure were reviewed (RO/RW,
  32/64-bit, exclusive/shared, recovery/fast-path);
- [ ] failure paths return the engine's own codes; nothing is swallowed or
  silently defaulted;
- [ ] comments are true (no unverified rationale), no dead or commented-out
  code, no leftover debug output;
- [ ] the hot path is untouched, or its cost is accounted for in the commit
  (§8.4);
- [ ] `-Wall -Wextra -Werror` clean and `tests/ci/check-clang-format.sh` clean;
- [ ] every moved/renamed path is updated in comments, docs and lint pins.

**Tests** (details in [tests/AGENTS.md](tests/AGENTS.md), "Pre-publish
checklist (tests)"):

- [ ] the test fails on the unfixed code and passes on the fixed one, and both
  host-width legs are proven;
- [ ] real library code, no simulacra, no `src/` inclusion outside the testing
  API;
- [ ] hygiene guards cover every exit path; `test_hygiene_check` is green;
- [ ] timeout entry present, manifest sorted, in-test budgets scaled;
- [ ] the unit is order-independent and visible in the intended CI scope
  (`ctest -N -L '<label>'`).

**Process:**

- [ ] the history is folded (no "fix"/"WIP" commits), one cause per commit;
- [ ] the ChangeLog entry follows §7 (user-observable wording, ≤100 characters,
  issue links);
- [ ] the PR references every related issue and says `Closes #N` when it
  completes one; its body carries the verification evidence — what was run and
  what was not;
- [ ] the verification levels of §9.1 are reached that the change warrants;
- [ ] the PR is a draft until the work is complete and CI is green (§5);
- [ ] the worktree, temporary branches and temporary files are removed.
