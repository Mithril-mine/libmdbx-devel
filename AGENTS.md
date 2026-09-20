
## Testing instructions
 - use CMake for build and CTest for testing
 - use at least Linux and Windows both environments for build and testing

## Code style
 - use LLVM codestyle

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
