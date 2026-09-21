
## Testing instructions
 - use CMake for build and CTest for testing
 - use at least Linux and Windows both environments for build and testing

## Code style
 - use LLVM codestyle

## Workspace & git-flow (internal dev machine)
 - The dev machine hosts a shared bare `origin` at `/sourcecraft/workspace/local-origin`
   and one working clone ("nook") per agent: `/sourcecraft/workspace/nook-<slug>`.
   Full description: `/sourcecraft/workspace/AGENT-WORKSPACE.md` (outside any repo).
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
 - Follow the integrated agent methodology in `skynet/superpowers/README.md` (adapted from
   obra/superpowers, MIT): brainstorming, writing-plans, executing-plans, TDD,
   systematic-debugging, code-review skills.
 - Segmented test execution (choose by change area and risk; do NOT run the whole
   corpus for superficial changes). Unit tests use a hierarchical label tree:
   every test carries the root `ut` label plus area labels `ut.api`, `ut.cxx`,
   `ut.env`, `ut.dbi`, `ut.txn`, `ut.cursor`, `ut.gc`, `ut.issues` (a test may
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
   See `skynet/superpowers/verification-before-completion/SKILL.md`.
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
