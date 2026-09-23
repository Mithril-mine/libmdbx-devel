
## Testing instructions
 - use CMake for build and CTest for testing
 - use at least Linux and Windows both environments for build and testing
 - launch local builds with `nice 5` and tests with `nice 10` (share the host
   fairly; agents run concurrently on one machine)
 - roles & skills registry: `skynet/skills-roles.md`
 - **do NOT enable LTO in test builds**: configure test build dirs with
   `-DINTERPROCEDURAL_OPTIMIZATION=OFF`. LTO re-optimizes the whole library per
   target (~50x slower; we test the code, not the compiler). LTO stays ON only
   for release/library/tools builds that explicitly require it.
   See `skynet/build.md` §6.7.
 - **enable ccache in test builds** (TASK-23 policy, 2026-09-23): pass
   `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`
   on every test-build configure, with shared
   `CCACHE_DIR=${SKYNET_ROOT}/.skynet/ccache` so all nooks share one cache.
   See `skynet/build.md` §6.7.

## Code style
 - use LLVM codestyle

## Workspace & git-flow (internal dev machine)
 - The dev machine hosts a shared bare `origin` at `${SKYNET_ROOT}/local-origin`
   and one working clone ("nook") per agent: `${SKYNET_ROOT}/nook-<slug>`.
   Full description: `${SKYNET_ROOT}/AGENT-WORKSPACE.md` (outside any repo).
 - Model: `working` → `devel` → `master` → `stable` → `lts`. Agents branch from
   `devel`, push feature branches to the local origin, merge into `devel` after
   review, and only publish outward (SourceCraft `upstream`, GitHub mirror) per
   the rules in that document.
 - In every nook: `origin` = local origin, `upstream` = SourceCraft, `github` =
   GitHub mirror. Keep the local origin in sync with `upstream` before publishing.

## Project index & agent practices
 - Start from the project index: `skynet/README.md` (repo map, build, architecture, workflows,
   testing infrastructure, amalgamation).
 - Platform specifics (SourceCraft CI/MCP/PR flow): `skynet/sourcecraft/README.md`.
 - Follow the integrated agent methodology in `skynet/skills/superpowers/README.md`
   (adapted from obra/superpowers, MIT): brainstorming, writing-plans, executing-plans,
   TDD, systematic-debugging, code-review skills.
 - Segmented test execution (choose by change area and risk; do NOT run the whole
   corpus for superficial changes). Unit tests use a hierarchical label tree:
   every test carries the root `ut` label plus area labels `ut.api`, `ut.cxx`,
   `ut.env`, `ut.dbi`, `ut.txn`, `ut.cursor`, `ut.gc`, `ut.issues`, `ut.tools` (a test may
   belong to several branches and runs once), and slow stress tests carry the
   orthogonal attribute `ut.heavy`:

   - P0 targeted:  `ctest -R '^<test>$'` (seconds) — for the specific change.
   - P1 fast ut:   `ctest -L 'ut\.' -LE 'ut\.heavy'` (~1.5 min) — default for
     routine changes; run on both compilers when headers are touched.
   - P2 full ut:   `ctest -L 'ut\.'` (~3.5 min) — when changing ut-covered areas.
   - P3 domain:    `ctest -L 'ut\.dbi'` / `ut\.cxx` / `ut\.cursor` ... — for a
     specific area, including all its subtree branches.
   - P4 full local:`ctest` (+ smoke, ~10+ min) — only when touching the core (src/).
   - P5 milestone: asan/ubsan/stochastic + GitHub CI — by explicit approval.
See `skynet/skills/superpowers/verification-before-completion/SKILL.md`.
  - Integrity checks: every `mdbx_chk`-based test (`*_chk`) carries the orthogonal
    `chk` label (B26). Routine runs exclude it (`-LE 'ut\.heavy|chk'`, as emitted by
    `tests/select-tests.sh`); plain `ctest` (P4 full) still includes it; to run only
    the integrity checks use `ctest -L chk`. Sync-mode policy (B27): mdbx_test
    scenarios whose durability is not the point use `--mode=+nosync-safe` (e.g.
    `smoke_basic`, `smoke_t1_hill/copy`); the durable variants (`smoke_writemap`,
    `smoke_fault`, `smoke_t1_nested`, the T3/long family) keep real sync.
  - Impact-based selection: instead of hand-picking the level, map changed paths
    to the affected labels with `tests/select-tests.sh` (build dir: run the
    printed `ctest -L ... -LE 'ut\.heavy'`, or `--run` to execute). Sources under
    `tests/ut/<area>/` map to their `ut.<area>` label; core (`src/`, `mdbx.h`,
    `mdbx++/`) pulls in `ut\.|smoke-t1|smoke-t2`; build files (`cmake/`,
    `CMakeLists.txt`, `GNUmakefile`) pull in smoke + full ut.
  - mdbx_test (`tests/framework/`) is STOCHASTIC — a fixed `--prng-seed` gives a
    reproducible per-actor operation sequence (multi-process interleaving is not).
    Use bounded `--nops` and fixed seeds for fast profiles; attach the seed to any
    bug report. Details: `tests/mdbx-test-options.md`.
  - Tiered smoke via CTest labels and GNUmakefile targets (`make smoke-t1/t2/t3`):
    - T1 `smoke-t1` — fast deterministic smoke, seconds, fixed seeds (`--nops`,
      `+nosync-safe`), never long iterations.
    - T2 `smoke-t2` — medium smoke (the quick-smoke family).
    - T3 `smoke-t3` — long/full stochastic runs (milestone/nightly; needs
      `cmake-stochastic-build`/`MDBX_ENABLE_LONG_TESTS`).
  - CI policy: SourceCraft CI quota is exhausted. GitHub CI is slow and expensive —
   trigger it only at milestones (agent proposal + user confirmation / explicit
   instruction), not for routine commits.
 - Keep build-file changes out of the amalgamated `dist/` (dist-cutoff markers), see
   `skynet/build.md` section on amalgamation.
 - Maintain the codebase knowledge graph (memory-MCP) per `skynet/memory-codex.md`:
   verify observations against code before acting (Gemba), refresh entities after
   significant work, record invariants/naming traps with provenance.
 - Delivery contract (§27 protocol): deliver only locally validated results; every
   REPORT/merge request MUST carry a validation matrix (`VALIDATED: <toolchain> <level> N/N`)
   — the coordinator does NOT re-run your builds/tests; cross-platform risks are
   absorbed by CI. Do not duplicate others' verification (Kaizen).
 - Domain reviewers (§28): branches are routed to specialized reviewers
   (`review-cmake`, `review-cpp`, `review-win`, `review-macos`) via `REV:<domain>`
   letters; address their verdicts before requesting merge.
 - Build-deps hygiene (§29): everything read/written during build & tests must be
   declared in CMake/Ninja dependencies; periodic strace audit keeps nothing
   "out of control".
- Backport policy (§30): bugfixes found on devel are backported to stable
    branches (master, lts/0.13, later v0.15.x) per the targeted-API-test criterion:
    test passes → skip; builds-but-fails → backport/fix separately; doesn't-build →
    investigate, ask the owner if the API predates or deeper issues exist.
  - Session model (§25a/25b protocol): the host runs a hard pool of at most 4
    concurrent busy headless instances (+1 interactive coordinator). Wake gate
    is memory-only (load-average gate removed 2026-09-23 — slow feedback
    oscillator that evicted agents during builds/tests; builds already run
    under `nice`, MAX_BUSY bounds concurrency). Work
    happens in a shared pool of 5 sandboxes `nook-pool-1..5` (roles are mapped
    dynamically, not per-nook). Each role has ONE canonical opencode session
    (`ses_...`) recorded in `.skynet/sessions.json`; orchestrator resumes it via
    `-s` in the assigned pool nook and auto-captures new ids after a fresh boot.
    Agents mirror their real id as `opencode_session_id=ses_...` in their entity.
    Never run bare `-c`. Do not create ad-hoc extra instances of a role while its
    first instance is busy.
